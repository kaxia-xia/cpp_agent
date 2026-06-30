// coding-agent: a Linux terminal coding agent powered by DeepSeek / Zhipu GLM.
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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

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
  /exit        quit
  .  on its own line (or Ctrl-D) to submit a prompt
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
        "You are coding-agent, an autonomous software-engineering assistant running in a Linux "
        "terminal. Your job is to help the user write, understand, refactor, and debug code.\n\n"
        "WORKSPACE\n  root: {}\n  os:   Linux\n\n"
        "TOOLS available (call via function/tool calling):\n"
        "  - read_file(path): read a text file under the workspace.\n"
        "  - write_file(path, content): create/overwrite a file; parent dirs auto-created.\n"
        "  - list_dir(path?): list directory entries as '<type> <name>' lines.\n"
        "  - run_command(command, timeout?): run a shell command (/bin/sh -c) in the workspace root; "
        "returns combined stdout+stderr and exit code. Use for builds, tests, git, grep.\n"
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

struct TurnOutcome {
    std::string final_text;
    long prompt_tokens = 0;
    long completion_tokens = 0;
};

TurnOutcome run_turn(http::Client& http, const llm::Provider& provider,
                     const Config& cfg, std::vector<llm::Message>& messages,
                     const json::Value& tools_json, const std::string& api_key) {
    TurnOutcome out;

    std::string last_fingerprint;
    int same_streak = 0;
    constexpr int kMaxSameStreak = 3;

    for (int iter = 0; iter < cfg.max_iterations; ++iter) {
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
            try {
                res = llm::chat_completion_stream(
                    http, provider, opts, messages, tools_json, api_key,
                    [&](std::string_view text_delta,
                        const std::vector<llm::ToolCall>& tool_calls_so_far) {
                        // Show text as it streams.
                        if (!text_delta.empty()) {
                            printer.feed(text_delta);
                        }
                        // If tool calls appeared in the stream, show them immediately.
                        if (!tool_calls_so_far.empty()) {
                            has_tool_calls = true;
                        }
                    });
                break;
            } catch (const std::exception& e) {
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

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) return 2;

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

    http::Client http;
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
        try {
            TurnOutcome out = run_turn(http, provider, cfg, messages, tools_json, api_key);
            total_prompt += out.prompt_tokens;
            total_completion += out.completion_tokens;
            std::cerr << std::format("\n[tokens: in={}, out={}]\n", total_prompt, total_completion);
        } catch (const std::exception& e) {
            print_error(e.what());
            return 1;
        }
        return 0;
    }

    std::cout << std::format("coding-agent  provider={}  model={}  root={}\n",
                             provider.name,
                             cfg.model.empty() ? provider.default_model : cfg.model,
                             cfg.root.string());
    std::cout << "Type your prompt; submit with a line containing only '.', or Ctrl-D.\n";
    std::cout << "Commands: /help /model /provider /clear /tokens /exit\n\n";

    std::string line;
    while (true) {
        set_color("35"); std::cout << "> " << std::flush; reset_color();
        std::string prompt;
        bool got_any = false;
        while (std::getline(std::cin, line)) {
            if (line == ".") { break; }
            if (!prompt.empty()) prompt.push_back('\n');
            prompt += line;
            got_any = true;
        }
        if (!got_any && std::cin.eof()) { std::cout << "\n"; break; }
        if (prompt.empty()) continue;

        if (prompt.size() > 1 && prompt.front() == '/' && prompt.find('\n') == std::string::npos) {
            std::string cmd = prompt;
            if (cmd == "/exit" || cmd == "/quit") break;
            if (cmd == "/help") { std::cout << kHelp; continue; }
            if (cmd == "/clear") {
                messages.clear();
                messages.push_back(llm::Message::system(build_system_prompt(cfg.root)));
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
            std::cout << "[unknown command: " << cmd << "  (try /help)]\n";
            continue;
        }

        messages.push_back(llm::Message::user(prompt));
        try {
            TurnOutcome out = run_turn(http, provider, cfg, messages, tools_json, api_key);
            total_prompt += out.prompt_tokens;
            total_completion += out.completion_tokens;
            std::cout << std::format("[{} in / {} out]\n", out.prompt_tokens, out.completion_tokens);
        } catch (const llm::LLMError& e) {
            print_error(e.what());
            if (!messages.empty() && messages.back().role == "user") messages.pop_back();
        } catch (const std::exception& e) {
            print_error(e.what());
        }
        std::cout << '\n';
    }
    return 0;
}
