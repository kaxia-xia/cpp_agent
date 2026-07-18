// coding-agent: a terminal coding agent powered by DeepSeek / Zhipu GLM.
//
// Build:   cmake -B build && cmake --build build
// Run:     DEEPSEEK_API_KEY=... ./build/coding-agent
//          ZHIPU_API_KEY=...   ./build/coding-agent --provider glm
//
// Single-shot: ./build/coding-agent --once "add a README"
//
#include "http.hpp"
#include "json.hpp"
#include "llm.hpp"
#include "markdown.hpp"
#include "tools.hpp"
#include "context.hpp"
#include "git.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <set>
#include <signal.h>
#include <termios.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ── Global interrupt flag ────────────────────────────────────────────
// Set to true by the SIGINT handler when the user presses Ctrl+C.
// Checked by the HTTP client's progress callback to abort in-flight
// requests, and by the main loop to decide whether to exit or just
// cancel the current turn.
static std::atomic<bool> g_interrupted{false};

// SIGINT handler: set the interrupt flag.
// In REPL mode, if we are waiting for user input (not in a turn),
// the default SIGINT behaviour (exit) is preserved by checking the
// flag at the right places.
extern "C" void handle_sigint(int) {
    g_interrupted.store(true, std::memory_order_release);
}

namespace {

// ── Pause/Resume (Ctrl+S / Ctrl+Q) ───────────────────────────────────
//
// We use the kernel's built-in IXON flow control.  No raw mode,
// no background thread, no interference with stdin.
// Just ensure IXON is enabled on the terminal.
bool ensure_ixon() {
    if (!isatty(STDIN_FILENO)) return false;
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return false;
    if (t.c_iflag & IXON) return true;  // already enabled

    t.c_iflag |= IXON;
    t.c_cc[VSTART] = 0x11;  // Ctrl+Q
    t.c_cc[VSTOP]  = 0x13;  // Ctrl+S
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
    return true;
}

// ── Config & helpers ─────────────────────────────────────────────────

struct Config {
    std::string provider_name = "deepseek";
    std::string model;
    std::string api_key;
    fs::path root = fs::current_path();
    double temperature = 0.3;
    std::optional<int> max_tokens;
    int max_iterations = 100;
    bool once = false;
    std::string initial_prompt;
};

constexpr char const* kHelp =
R"(coding-agent - terminal coding agent (DeepSeek / Zhipu GLM)

USAGE
  coding-agent [OPTIONS] [--once "prompt"]

OPTIONS
  -p, --provider <name>   deepseek | glm            (default: deepseek)
  -m, --model <name>       model id, e.g. deepseek-chat, glm-4.5
      --api-key <key>      API key (else read from env, see below)
  -r, --root <dir>         workspace root            (default: cwd)
  -t, --temperature <f>    0.0 - 2.0                 (default: 0.3)
      --max-tokens <n>     max output tokens
      --max-iters <n>      agent loop cap            (default: 100)
      --once "prompt"       run one task then exit
  -h, --help               show this help

ENV
  DEEPSEEK_API_KEY         key for the deepseek provider
  ZHIPU_API_KEY            key for the glm provider
  CODING_AGENT_PROVIDER    default provider override
  CODING_AGENT_MODEL       default model override

REPL COMMANDS
  /help        show commands
  /model NAME  switch model
  /provider N  switch provider (deepseek|glm)
  /clear       reset conversation
  /tokens      show token usage totals
  /snap [lbl]  save a labeled checkpoint of the current context
  /versions    list saved context versions (/snaps, /history)
  /back <id>   roll back the context to version <id>
  /undo        undo the last turn (roll back one version)
  /exit        quit
  .  on its own line (or Ctrl-D) to submit a prompt

FLOW CONTROL
  Ctrl+S      pause output (terminal flow control)
  Ctrl+Q      resume output
  Ctrl+C      during AI response: cancel, preserve context, return to prompt
              at prompt: exit the program
)";

bool is_tty() { return ::isatty(STDOUT_FILENO) != 0; }

void set_color(std::string_view code) {
    if (is_tty()) std::cout << "\033[" << code << 'm';
}
void reset_color() {
    if (is_tty()) std::cout << "\033[0m";
}

// Streaming-aware printer: renders markdown incrementally as text arrives.
struct StreamPrinter {
    std::string buffer;
    bool in_code_block = false;

    void feed(std::string_view text) {
        for (char c : text) {
            buffer.push_back(c);
            if (buffer.size() >= 3) {
                std::string end = buffer.substr(buffer.size() - 3);
                if (end == "```") in_code_block = !in_code_block;
            }
            if (c == '\n' && !in_code_block && buffer.size() > 1) {
                auto last_newline = buffer.find_last_of('\n', buffer.size() - 2);
                if (last_newline != std::string::npos) {
                    std::string to_print(buffer.begin(), buffer.begin() + static_cast<long>(last_newline) + 1);
                    std::string rest(buffer.begin() + static_cast<long>(last_newline) + 1, buffer.end());
                    markdown::render(to_print);
                    buffer = rest;
                }
            }
        }
    }

    void flush() {
        if (!buffer.empty()) {
            markdown::render(buffer);
            buffer.clear();
        }
    }
};

void print_tool_call(std::string_view name, std::string_view args) {
    set_color("33");
    std::cout << "[tool] " << name;
    if (!args.empty()) {
        std::string a(args);
        std::string preview = a.size() > 200 ? (a.substr(0, 200) + "...") : a;
        std::cout << " " << preview;
    }
    std::cout << '\n';
    reset_color();
}

void print_tool_result(std::string_view result) {
    set_color("2");
    std::string r(result);
    std::string preview = r.size() > 400 ? (r.substr(0, 400) + "...") : r;
    std::string line;
    for (char c : preview) {
        if (c == '\n') { std::cout << "    " << line << '\n'; line.clear(); }
        else line.push_back(c);
    }
    if (!line.empty()) std::cout << "    " << line << '\n';
    reset_color();
}

void print_error(std::string_view msg) {
    set_color("31");
    std::cout << "[error] " << msg << '\n';
    reset_color();
}

bool parse_args(int argc, char** argv, Config& cfg) {
    auto next = [&](int& i) -> std::optional<std::string> {
        if (i + 1 >= argc) return std::nullopt;
        return std::string(argv[++i]);
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto grab = [&]() -> std::optional<std::string> { return next(i); };

        if (a == "-h" || a == "--help") { std::cout << kHelp; std::exit(0); }
        else if (a == "-p" || a == "--provider") {
            if (auto v = grab()) cfg.provider_name = *v; else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "-m" || a == "--model") {
            if (auto v = grab()) cfg.model = *v; else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "--api-key") {
            if (auto v = grab()) cfg.api_key = *v; else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "-r" || a == "--root") {
            if (auto v = grab()) cfg.root = *v; else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "-t" || a == "--temperature") {
            if (auto v = grab()) {
                try { cfg.temperature = std::stod(*v); }
                catch (...) { std::cerr << "bad temperature: " << *v << "\n"; return false; }
            } else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "--max-tokens") {
            if (auto v = grab()) {
                try { cfg.max_tokens = std::stoi(*v); }
                catch (...) { std::cerr << "bad max-tokens: " << *v << "\n"; return false; }
            } else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "--max-iters") {
            if (auto v = grab()) {
                try { cfg.max_iterations = std::stoi(*v); }
                catch (...) { std::cerr << "bad max-iters: " << *v << "\n"; return false; }
            } else { std::cerr << "missing value for " << a << "\n"; return false; }
        } else if (a == "--once") {
            cfg.once = true;
            if (auto v = grab()) cfg.initial_prompt = *v;
            else if (i + 1 < argc && argv[i+1][0] != '-') cfg.initial_prompt = argv[++i];
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "unknown option: " << a << "\n"; return false;
        } else {
            cfg.initial_prompt = a;
            cfg.once = true;
        }
    }
    if (const char* p = std::getenv("CODING_AGENT_PROVIDER")) cfg.provider_name = p;
    if (cfg.model.empty()) {
        if (const char* m = std::getenv("CODING_AGENT_MODEL")) cfg.model = m;
    }
    return true;
}

std::string resolve_api_key(const Config& cfg, const llm::Provider& p) {
    if (!cfg.api_key.empty()) return cfg.api_key;
    if (const char* k = std::getenv(p.api_key_env.c_str())) return k;
    return {};
}

std::string build_system_prompt(const fs::path& root) {
    return std::format(
        "当前环境为android termux，基于这一点处理接下来的对话。\n\n"
        "You are coding-agent, an autonomous software-engineering assistant running in a Linux "
        "terminal (Android Termux). Your job is to help the user write, understand, refactor, and debug code.\n\n"
        "WORKSPACE\n  root: {}\n  os:   Android (Termux)\n\n"
        "TOOLS available (call via function/tool calling):\n"
        "  - read_file(path, offset?, limit?, max_bytes?): read file with optional byte offset/limit for large files.\n"
        "  - write_file(path, content): create/overwrite a file; parent dirs auto-created.\n"
        "  - list_dir(path?): list directory entries.\n"
        "  - run_command(command, timeout?): run a shell command.\n"
        "  - edit_file(path, old_text, new_text): replace first occurrence of old_text with new_text.\n"
        "  - delete_file(path): delete a file or empty directory.\n"
        "  - rename_file(source, destination): rename/move a file or directory.\n"
        "  - copy_file(source, destination): copy a file or directory.\n"
        "  - append_file(path, content): append content to end of file.\n"
        "  - search_text(pattern, path?, regex?, max_bytes?): grep for pattern in files.\n"
        "  - find_files(pattern, path?): find files matching a glob pattern.\n"
        "  - file_info(path): get file/directory metadata.\n"
        "  - read_multiple_files(paths, max_bytes?): read multiple files at once.\n"
        "  - write_multiple_files(files): write multiple files at once.\n"
        "  - fetch_url(url, timeout?, max_bytes?): HTTP GET a URL and return the body.\n"
        "  - parse_html(html, query?): parse HTML, extract text/links/CSS-selector matches.\n"
        "  - parse_xml(xml, xpath?): parse XML, extract text/structure/XPath matches.\n"
        "  - parse_json(json, query?): parse and query JSON with dot-separated paths.\n"
        "  - render_mermaid(mermaid, output): convert Mermaid diagram to SVG file.\n"
        "  - image_info(path): get image metadata (format, dimensions, mode).\n"
        "  - image_convert(source, destination, width?, height?, quality?): convert/resize images.\n"
        "  - image_to_svg(source, destination): embed bitmap image as base64 in SVG.\n"
        "  - clipboard(action, content?): read/write Android system clipboard.\n"
        "  - notify(title, content, priority?, alert_once?, sound?): send Android notification (use sound=true to play a sound).\n"
        "  - speak(text, lang?): text-to-speech on device.\n"
        "  - vibrate(duration_ms?): vibrate the device.\n"
        "  - run_python(code, timeout?): execute Python code snippet.\n"
        "  - ocr(image_path, lang?): OCR text from image using Tesseract.\n"
        "  - qr_encode(data, output): generate QR code image.\n"
        "  - qr_decode(image_path): decode QR/barcode from image.\n"
        "  - diff_files(file1, file2, context_lines?): compare two files.\n"
        "  - compress(source, output, format?): create zip/tar.gz archive.\n"
        "  - decompress(archive, output_dir?): extract archive.\n"
        "  - system_info(category?): get device info (battery/cpu/memory/storage/network).\n"
        "  - weather(location?): query weather forecast.\n"
        "  - screenshot(output?): capture device screen.\n"
        "  - plot_chart(chart_type, data_json, title?, output?): generate chart image.\n"
        "  - finish(summary): return the final answer and end the task. Call exactly once when done.\n\n"
        "GUIDELINES\n"
        "  1. Explore first: list_dir/read_file before making changes so you understand the codebase.\n"
        "  2. Make real changes with write_file; do not just paste diffs at the user.\n"
        "  3. Verify: run_command builds/tests to confirm your changes work; fix what breaks.\n"
        "  4. Keep tool outputs concise. Prefer a few focused reads over dumping huge files.\n"
        "  5. Paths are sandboxed to the workspace root; relative paths are preferred.\n"
        "  6. When the task is complete, call finish with a short summary of what you did.\n"
        "  7. If a tool errors, read the message and adapt — do not repeat the identical call.\n"
        "  8. Never invent file contents; read first when you need accuracy.\n",
        fs::weakly_canonical(root).string());
}

// Derive a short label for an auto-snapshot from the user's prompt.
std::string snap_label(const std::string& prompt) {
    auto nl = prompt.find('\n');
    std::string first = (nl == std::string::npos) ? prompt : prompt.substr(0, nl);
    auto a = first.find_first_not_of(" \t");
    if (a == std::string::npos) return "turn";
    auto b = first.find_last_not_of(" \t\r");
    first = first.substr(a, b - a + 1);
    if (first.size() > 40) first = first.substr(0, 40) + "...";
    return first;
}

struct TurnOutcome {
    std::string final_text;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    bool interrupted = false;  // true if user pressed Ctrl+C during this turn
};

TurnOutcome run_turn(http::Client& http, const llm::Provider& provider,
                     const Config& cfg, std::vector<llm::Message>& messages,
                     const json::Value& tools_json, const std::string& api_key) {
    TurnOutcome out;

    std::string last_fingerprint;
    int same_streak = 0;
    constexpr int kMaxSameStreak = 3;

    for (int iter = 0; iter < cfg.max_iterations; ++iter) {
        // Check for Ctrl+C interrupt before each LLM call.
        if (g_interrupted.load(std::memory_order_acquire)) {
            out.interrupted = true;
            return out;
        }

        llm::CompletionOptions opts;
        opts.model = cfg.model.empty() ? provider.default_model : cfg.model;
        opts.temperature = cfg.temperature;
        opts.max_tokens = cfg.max_tokens;

        // Show provider prefix.
        set_color("2");
        std::cerr << std::format("[{}] ", provider.label);
        reset_color();
        std::cerr << std::flush;

        llm::CompletionResult res;
        constexpr int kTurnRetries = 3;
        StreamPrinter printer;
        bool has_tool_calls = false;

        for (int turn_attempt = 1; turn_attempt <= kTurnRetries; ++turn_attempt) {
            // Check for interrupt before retry.
            if (g_interrupted.load(std::memory_order_acquire)) {
                out.interrupted = true;
                return out;
            }

            try {
                res = llm::chat_completion_stream(
                    http, provider, opts, messages, tools_json, api_key,
                    [&](std::string_view text_delta,
                        const std::vector<llm::ToolCall>& tool_calls_so_far) {
                        // Show text as it streams.
                        if (!text_delta.empty()) {
                            printer.feed(text_delta);
                        }
                        if (!tool_calls_so_far.empty()) {
                            has_tool_calls = true;
                        }
                    });
                break;
            } catch (const std::exception& e) {
                // If the user interrupted, don't retry.
                if (g_interrupted.load(std::memory_order_acquire)) {
                    out.interrupted = true;
                    return out;
                }
                std::string what = e.what();
                if (what.find("interrupted by user") != std::string::npos) {
                    out.interrupted = true;
                    return out;
                }
                if (!llm::is_transient(e)) throw;
                if (turn_attempt < kTurnRetries) {
                    int wait = 30 * turn_attempt;
                    set_color("33");
                    std::cerr << std::format(
                        "\n[retry] iter {}: transient failure ({}); "
                        "waiting {}s before retry {}/{}\n",
                        iter + 1, e.what(), wait, turn_attempt + 1, kTurnRetries);
                    reset_color();
                    std::this_thread::sleep_for(std::chrono::seconds(wait));
                } else {
                    throw;
                }
            }
        }

        printer.flush();
        std::cout << std::flush;

        out.prompt_tokens += res.prompt_tokens;
        out.completion_tokens += res.completion_tokens;
        messages.push_back(res.assistant);

        if (res.assistant.tool_calls.empty()) {
            out.final_text = res.assistant.content.value_or("");
            std::cout << '\n';
            return out;
        }

        // Print tool calls and execute them.
        std::cout << '\n';
        std::string fingerprint;
        fingerprint.reserve(64);
        for (const auto& tc : res.assistant.tool_calls) {
            fingerprint += tc.function_name;
            fingerprint += '|';
            fingerprint += tc.arguments;
            fingerprint += '\x1f';
        }
        if (fingerprint == last_fingerprint) {
            ++same_streak;
        } else {
            same_streak = 1;
            last_fingerprint = fingerprint;
        }

        for (const auto& tc : res.assistant.tool_calls) {
            // Check for interrupt before executing each tool.
            if (g_interrupted.load(std::memory_order_acquire)) {
                out.interrupted = true;
                return out;
            }

            print_tool_call(tc.function_name, tc.arguments);
            std::string result = tools::execute(tc.function_name, tc.arguments, cfg.root);
            print_tool_result(result);
            messages.push_back(llm::Message::tool_result(tc.id, tc.function_name, result));

            if (tc.function_name == "finish") {
                out.final_text = result;
                return out;
            }
        }

        if (same_streak >= kMaxSameStreak) {
            set_color("33");
            std::cerr << std::format(
                "[warn] agent appears stuck: identical tool calls repeated "
                "{} times in a row at iter {}/{}. Breaking early.\n",
                same_streak, iter + 1, cfg.max_iterations);
            reset_color();
            out.final_text = std::format(
                "[agent stopped: identical tool calls repeated {} times "
                "without making progress. Last tool: {}. "
                "Consider rephrasing the task or raising --max-iters.]",
                same_streak,
                res.assistant.tool_calls.front().function_name);
            return out;
        }
    }
    out.final_text = "[agent hit iteration limit without calling finish]";
    return out;
}

// ── Git integration ───────────────────────────────────────────────────

void init_git(const fs::path& root) {
    if (!git::is_available()) {
        std::cerr << std::format(
            "[git] warning: git is not installed or not on PATH.\n"
            "[git] file changes will NOT be versioned automatically.\n"
            "[git] install git to enable automatic snapshots and easy undo.\n");
        return;
    }

    if (!git::ensure_repo(root)) {
        std::cerr << std::format(
            "[git] warning: could not initialise git repository.\n");
    }
}

std::string auto_commit(const fs::path& root, std::string_view label,
                 const std::set<std::string>& dirty_before) {
    if (!git::is_available()) return {};
    set_color("2");
    std::cerr << "[git] committing changes... " << std::flush;
    reset_color();
    return git::commit_changes(root, label, dirty_before);
    std::cerr << "\r\033[K";
    std::cerr << std::flush;
}

// ── REPL input reader ────────────────────────────────────────────────

bool read_prompt(std::string& prompt) {
    prompt.clear();
    bool got_any = false;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (line == ".") {
            break;
        }
        if (!prompt.empty()) prompt.push_back('\n');
        prompt += line;
        got_any = true;
    }

    if (std::cin.eof()) {
        std::cin.clear();
        if (!got_any) return false;
    }

    return got_any;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) return 2;

    // ── Install SIGINT handler ────────────────────────────────────
    // During AI response: Ctrl+C sets g_interrupted, which aborts the
    // HTTP request via libcurl's progress callback and causes run_turn
    // to return early.  At the prompt: Ctrl+C will be caught after
    // read_prompt returns (it will see g_interrupted and exit).
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    // ── Terminal flow control (Ctrl+S / Ctrl+Q) ───────────────────
    bool has_flow_control = false;
    if (isatty(STDIN_FILENO)) {
        has_flow_control = ensure_ixon();
    }

    llm::Provider provider;
    if (auto* p = llm::find_provider(cfg.provider_name)) {
        provider = *p;
    } else {
        std::cerr << std::format("unknown provider '{}'. use deepseek|glm\n", cfg.provider_name);
        return 2;
    }

    std::string api_key = resolve_api_key(cfg, provider);
    if (api_key.empty()) {
        std::cerr << std::format(
            "error: no API key. set ${} or pass --api-key.\n", provider.api_key_env);
        return 2;
    }

    std::error_code ec;
    cfg.root = fs::weakly_canonical(cfg.root, ec);
    if (ec) {
        std::cerr << std::format("error: invalid root '{}': {}\n", cfg.root.string(), ec.message());
        return 2;
    }

    // ── Git startup check ─────────────────────────────────────────
    init_git(cfg.root);

    http::Client http;
    // Pass the interrupt flag to the HTTP client so libcurl's progress
    // callback can abort in-flight requests when the user presses Ctrl+C.
    http.set_interrupt_flag(&g_interrupted);

    const json::Value tools_json = tools::tool_schemas();

    std::vector<llm::Message> messages;
    messages.push_back(llm::Message::system(build_system_prompt(cfg.root)));

    long total_prompt = 0, total_completion = 0;

    if (cfg.once) {
        if (cfg.initial_prompt.empty()) {
            std::cerr << "error: --once requires a prompt.\n";
            return 2;
        }
        messages.push_back(llm::Message::user(cfg.initial_prompt));

        std::set<std::string> dirty_before;
        if (git::is_available()) {
            dirty_before = git::get_dirty_files(cfg.root);
        }

        try {
            TurnOutcome out = run_turn(http, provider, cfg, messages, tools_json, api_key);
            total_prompt += out.prompt_tokens;
            total_completion += out.completion_tokens;
            std::cerr << std::format("\n[tokens: in={}, out={}]\n", total_prompt, total_completion);
        } catch (const std::exception& e) {
            print_error(e.what());
            auto_commit(cfg.root, snap_label(cfg.initial_prompt), dirty_before);
            return 1;
        }
        auto_commit(cfg.root, snap_label(cfg.initial_prompt), dirty_before);
        return 0;
    }

    // ── Initialise history with the current HEAD hash ────────────
    context::History history;
    {
        std::string init_hash;
        if (git::is_available()) {
            init_hash = git::get_head_hash(cfg.root);
        }
        history.save(messages, "init", init_hash);
    }

    std::cout << std::format("coding-agent  provider={}  model={}  root={}\n",
                             provider.name,
                             cfg.model.empty() ? provider.default_model : cfg.model,
                             cfg.root.string());
    std::cout << "Type your prompt; submit with a line containing only '.', or Ctrl-D.\n";
    std::cout << "Commands: /help /model /provider /clear /tokens /snap /versions /back /undo /exit\n";
    if (has_flow_control) {
        std::cout << "Flow: Ctrl+S pause  Ctrl+Q resume  (terminal flow control)\n";
    } else {
        std::cout << "Note: Ctrl+S/Ctrl+Q flow control unavailable (not a terminal or IXON disabled)\n";
    }
    std::cout << "Interrupt: Ctrl+C during AI response to cancel (context preserved), at prompt to exit\n";
    std::cout << '\n';

    std::string prompt;
    while (true) {
        // Reset the interrupt flag at the start of each prompt cycle.
        g_interrupted.store(false, std::memory_order_release);

        // Print the prompt indicator.
        set_color("35"); std::cout << "> " << std::flush; reset_color();

        // Read user input.
        if (!read_prompt(prompt)) {
            // True EOF (Ctrl-D on empty line) → exit.
            std::cout << "\n";
            break;
        }

        // If the user pressed Ctrl+C during read_prompt (no input was read),
        // treat it as exit.
        if (prompt.empty() && g_interrupted.load(std::memory_order_acquire)) {
            std::cout << "\n";
            break;
        }

        if (prompt.empty()) continue;

        // ── Handle slash commands ─────────────────────────────────
        if (prompt.size() > 1 && prompt.front() == '/' && prompt.find('\n') == std::string::npos) {
            std::string cmd = prompt;
            if (cmd == "/exit" || cmd == "/quit") break;
            if (cmd == "/help") { std::cout << kHelp; continue; }
            if (cmd == "/clear") {
                messages.clear();
                messages.push_back(llm::Message::system(build_system_prompt(cfg.root)));
                history.clear();
                std::string init_hash;
                if (git::is_available()) {
                    init_hash = git::get_head_hash(cfg.root);
                }
                history.save(messages, "init", init_hash);
                std::cout << "[conversation cleared]\n";
                continue;
            }
            if (cmd == "/tokens") {
                std::cout << std::format("[totals: prompt_in={} completion_out={}]\n",
                                         total_prompt, total_completion);
                continue;
            }
            if (cmd.rfind("/model ", 0) == 0) {
                cfg.model = cmd.substr(7);
                std::cout << std::format("[model -> {}]\n", cfg.model.empty() ? provider.default_model : cfg.model);
                continue;
            }
            if (cmd.rfind("/provider ", 0) == 0) {
                std::string name = cmd.substr(10);
                if (auto* p = llm::find_provider(name)) {
                    provider = *p;
                    std::string k = resolve_api_key(cfg, provider);
                    if (k.empty()) {
                        std::cout << std::format("[provider set to {} but ${} is unset]\n",
                                                 provider.name, provider.api_key_env);
                    } else {
                        api_key = k;
                        std::cout << std::format("[provider -> {}, model={}]\n",
                                                 provider.name,
                                                 cfg.model.empty() ? provider.default_model : cfg.model);
                    }
                } else {
                    std::cout << "[unknown provider: " << name << "]\n";
                }
                continue;
            }
            if (cmd == "/snap" || cmd.rfind("/snap ", 0) == 0) {
                std::string label;
                if (cmd.size() > 6) label = markdown::trim(cmd.substr(6));
                int id = history.save(messages, label);
                std::cout << std::format("[snapshot #{} saved: {}]\n",
                                         id, label.empty() ? "(unlabeled)" : label);
                continue;
            }
            if (cmd == "/versions" || cmd == "/snaps" || cmd == "/history") {
                if (history.empty()) { std::cout << "[no snapshots yet]\n"; continue; }
                std::cout << history.describe();
                continue;
            }
            if (cmd == "/back" || cmd.rfind("/back ", 0) == 0) {
                std::string arg = cmd.size() > 6 ? markdown::trim(cmd.substr(6)) : "";
                int id = 0;
                bool ok = !arg.empty();
                if (ok) { try { id = std::stoi(arg); } catch (...) { ok = false; } }
                if (!ok) { std::cout << "[usage: /back <id>  (use /versions to list)]\n"; continue; }
                std::vector<llm::Message> restored;
                if (history.rollback(id, restored)) {
                    messages = std::move(restored);
                    std::cout << std::format("[rolled back to version #{} ({} messages)]\n",
                                             id, messages.size());
                    if (git::is_available()) {
                        std::string hash = history.get_commit_hash(id);
                        if (hash.empty()) {
                            hash = history.get_nearest_commit_hash(id);
                        }
                        if (!hash.empty()) {
                            if (git::reset_to_commit(cfg.root, hash)) {
                                std::cout << "[git] files restored to snapshot #" << id << "\n";
                            }
                        }
                    }
                }
                else {
                    std::cout << std::format("[no version with id {}  (use /versions to list)]\n", id);
                }
                continue;
            }
            if (cmd == "/undo") {
                std::string undo_hash;
                if (git::is_available()) {
                    undo_hash = history.get_previous_commit_hash();
                }

                std::vector<llm::Message> restored;
                if (history.undo(restored)) {
                    int cid = history.current_id();
                    messages = std::move(restored);
                    std::cout << std::format("[undone: back to version #{} ({} messages)]\n",
                                             cid, messages.size());
                    if (git::is_available() && !undo_hash.empty()) {
                        if (git::reset_to_commit(cfg.root, undo_hash)) {
                            std::cout << "[git] files restored to previous snapshot\n";
                        }
                    }
                }
                else {
                    std::cout << "[nothing to undo]\n";
                }
                continue;
            }
            std::cout << "[unknown command: " << cmd << "  (try /help)]\n";
            continue;
        }

        // ── Process user prompt ───────────────────────────────────
        messages.push_back(llm::Message::user(prompt));

        std::set<std::string> dirty_before;
        if (git::is_available()) {
            dirty_before = git::get_dirty_files(cfg.root);
        }

        bool turn_interrupted = false;
        try {
            TurnOutcome out = run_turn(http, provider, cfg, messages, tools_json, api_key);
            total_prompt += out.prompt_tokens;
            total_completion += out.completion_tokens;
            turn_interrupted = out.interrupted;
            if (!out.interrupted) {
                std::cout << std::format("[{} in / {} out]\n", out.prompt_tokens, out.completion_tokens);
            }
        } catch (const llm::LLMError& e) {
            print_error(e.what());
            if (!messages.empty() && messages.back().role == "user") messages.pop_back();
        } catch (const std::exception& e) {
            print_error(e.what());
        }

        // If the turn was interrupted by Ctrl+C, do NOT auto-commit
        // (the agent's changes may be partial/incomplete).  However,
        // PRESERVE the user's prompt and all prior conversation context
        // so the user can continue from where they left off.  Only
        // remove any partial assistant/tool messages that may have been
        // added before the interrupt (they are incomplete).
        if (turn_interrupted) {
            set_color("33");
            std::cout << "[cancelled - context preserved, you can continue]\n";
            reset_color();
            // Remove any partial assistant/tool messages that may
            // have been added before the interrupt, but KEEP the
            // user's prompt so the conversation context is preserved.
            while (!messages.empty() && messages.back().role != "user" && messages.back().role != "system") {
                messages.pop_back();
            }
            // Save a snapshot so the user can roll back if needed.
            std::string commit_hash = auto_commit(cfg.root, snap_label(prompt), dirty_before);
            if (!commit_hash.empty()) {
                history.save(messages, snap_label(prompt) + " (interrupted)", commit_hash);
            } else {
                history.save(messages, snap_label(prompt) + " (interrupted)");
            }
            continue;
        }

        // Auto-commit any file changes made during this turn.
        std::string commit_hash = auto_commit(cfg.root, snap_label(prompt), dirty_before);
        if (!commit_hash.empty()) {
            history.save(messages, snap_label(prompt), commit_hash);
        } else {
            history.save(messages, snap_label(prompt));
        }
        std::cout << '\n';
    }
    return 0;
}
