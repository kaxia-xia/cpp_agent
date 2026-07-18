// tools.hpp - agent tools (filesystem + shell) and their JSON schemas.
#pragma once
 
#include "json.hpp"
 
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
 
// environ is a global symbol (POSIX); declare it at global scope so it is not
// namespace-qualified.
extern char** environ;
 
namespace tools {
 
namespace fs = std::filesystem;
 
// Resolve `path` (relative or absolute) against `root`, rejecting escapes.
inline fs::path resolve_under_root(const fs::path& root, std::string_view path) {
    fs::path p(path);
    fs::path full = p.is_absolute() ? p : (root / p);
    // canonical/weakly_canonical handles ".." traversal.
    fs::path canon = fs::weakly_canonical(full);
    fs::path root_canon = fs::weakly_canonical(root);
    // Ensure canon starts with root_canon.
    auto rel = fs::relative(canon, root_canon);
    if (rel.empty() || rel.string().substr(0, 2) == "..") {
        throw std::runtime_error(std::format("path '{}' escapes workspace root '{}'",
                                             p.string(), root_canon.string()));
    }
    return canon;
}
 
struct ToolError : std::runtime_error {
    explicit ToolError(std::string msg) : std::runtime_error(std::move(msg)) {}
};
 
struct ShellResult {
    int exit_code = 0;
    std::string output;
    bool timed_out = false;
};
 
// Run a shell command via /bin/sh -c, capturing combined stdout+stderr,
// with a hard timeout. Returns output + exit code.
inline ShellResult run_shell(std::string_view cmd, const fs::path& cwd, int timeout_seconds) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        throw ToolError(std::format("pipe() failed: {}", std::strerror(errno)));
    }
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        close(pipefd[0]); close(pipefd[1]);
        throw ToolError("posix_spawn_file_actions_init failed");
    }
    // Child writes to pipefd[1]; its stdout & stderr both go there.
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
 
    // posix_spawn doesn't easily chdir pre-glibc-2.29; emulate with "cd".
    std::string wrapped = std::format("cd {} && {}", cwd.string(), cmd);
    const char* argv2[] = {"/bin/sh", "-c", wrapped.c_str(), nullptr};
 
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "/bin/sh", &fa, nullptr,
                          const_cast<char* const*>(argv2), ::environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        throw ToolError(std::format("posix_spawnp failed: {}", std::strerror(errno)));
    }
 
    ShellResult result;
    std::string buf;
    std::array<char, 4096> chunk{};
    bool timed_out = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
 
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        timeval tv{};
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { timed_out = true; break; }
        auto remain = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        tv.tv_sec  = static_cast<long>(remain.count() / 1000000);
        tv.tv_usec = static_cast<long>(remain.count() % 1000000);
        int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) { timed_out = true; break; }
        ssize_t n = read(pipefd[0], chunk.data(), chunk.size());
        if (n <= 0) break;
        buf.append(chunk.data(), static_cast<size_t>(n));
    }
    close(pipefd[0]);
 
    if (timed_out) {
        kill(pid, SIGKILL);
        result.timed_out = true;
        buf += std::format("\n[tool: timed out after {}s, killed]\n", timeout_seconds);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
 
    result.output = std::move(buf);
    return result;
}
 
// The OpenAI-style tool definitions exposed to the model.
inline json::Value tool_schemas() {
    json::Array tools;
 
    auto fn = [](std::string name, std::string desc, json::Object params) {
        json::Object t;
        t["type"] = json::Value{"function"};
        json::Object f;
        f["name"] = json::Value{std::move(name)};
        f["description"] = json::Value{std::move(desc)};
        f["parameters"] = json::Value{std::move(params)};
        t["function"] = json::Value{std::move(f)};
        return json::Value{std::move(t)};
    };
 
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root (or absolute)."};
        props["path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("read_file",
            "Read the full contents of a text file from the workspace.",
            std::move(p)));
    }
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root (or absolute)."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object content_p; content_p["type"] = json::Value{"string"};
        content_p["description"] = json::Value{"Full file contents to write (overwrites existing)."};
        props["content"] = json::Value{std::move(content_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path", "content"});
        tools.push_back(fn("write_file",
            "Create or overwrite a file with the given contents. Parent dirs are created.",
            std::move(p)));
    }
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Directory path relative to workspace root (defaults to root)."};
        path_p["default"] = json::Value{"."};
        props["path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("list_dir",
            "List entries of a directory. Each line: '<type> <name>' where type is dir|file|link.",
            std::move(p)));
    }
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object cmd_p; cmd_p["type"] = json::Value{"string"};
        cmd_p["description"] = json::Value{"Shell command to run with /bin/sh -c."};
        props["command"] = json::Value{std::move(cmd_p)};
        json::Object to_p; to_p["type"] = json::Value{"integer"};
        to_p["description"] = json::Value{"Timeout in seconds (default 60, max 300)."};
        to_p["default"] = json::Value{60};
        props["timeout"] = json::Value{std::move(to_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"command"});
        tools.push_back(fn("run_command",
            "Run a shell command in the workspace root. Returns combined stdout+stderr and exit code. "
            "Use for builds, tests, git, grep, etc. Long-running commands are killed at timeout.",
            std::move(p)));
    }
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object s_p; s_p["type"] = json::Value{"string"};
        s_p["description"] = json::Value{"Short summary of what was accomplished."};
        props["summary"] = json::Value{std::move(s_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"summary"});
        tools.push_back(fn("finish",
            "Signal that the task is complete and return the final answer to the user. "
            "Call this exactly once when done instead of continuing to chat.",
            std::move(p)));
    }
    return json::Value{std::move(tools)};
}
 
// Truncate a tool result so we don't blow up the context.
inline std::string truncate(std::string s, size_t max_bytes = 60000) {
    if (s.size() <= max_bytes) return s;
    return s.substr(0, max_bytes) + std::format("\n...[truncated, {}/{} bytes shown]",
                                               max_bytes, s.size());
}
 
// Dispatch a tool call. `arguments` is the raw JSON arguments string.
inline std::string execute(const std::string& name, std::string_view arguments,
                           const fs::path& root) {
    json::Value args;
    if (!arguments.empty()) {
        try { args = json::parse(arguments); }
        catch (const std::exception& e) {
            return std::format("[tool error: invalid arguments JSON: {}]", e.what());
        }
    }
 
    auto get_str = [&](std::string_view key) -> std::string {
        if (args.is_object() && args.contains(key) && args[key].is_string())
            return args[key].as_string();
        return {};
    };
    auto get_int = [&](std::string_view key, int def) -> int {
        if (args.is_object() && args.contains(key)) {
            if (args[key].is_number()) return static_cast<int>(args[key].as_number());
            if (args[key].is_string()) {
                try { return std::stoi(args[key].as_string()); } catch (...) {}
            }
        }
        return def;
    };
 
    try {
        if (name == "read_file") {
            std::string path = get_str("path");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());
            std::ifstream f(resolved, std::ios::binary);
            if (!f) return std::format("[error: cannot open: {}]", resolved.string());
            std::ostringstream ss; ss << f.rdbuf();
            return truncate(ss.str());
        }
        if (name == "write_file") {
            std::string path = get_str("path");
            std::string content = get_str("content");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            fs::create_directories(resolved.parent_path());
            std::ofstream f(resolved, std::ios::binary | std::ios::trunc);
            if (!f) return std::format("[error: cannot open for write: {}]", resolved.string());
            f << content;
            f.close();
            if (!f) return std::format("[error: write failed: {}]", resolved.string());
            return std::format("[ok: wrote {} bytes to {}]", content.size(), resolved.string());
        }
        if (name == "list_dir") {
            std::string path = get_str("path");
            if (path.empty()) path = ".";
            fs::path resolved = resolve_under_root(root, path);
            if (!fs::exists(resolved)) return std::format("[error: not found: {}]", resolved.string());
            if (!fs::is_directory(resolved)) return std::format("[error: not a directory: {}]", resolved.string());
            std::ostringstream ss;
            std::vector<std::string> names;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(resolved, ec)) {
                std::string type = "file";
                std::error_code ec2;
                if (e.is_directory(ec2)) type = "dir";
                else if (e.is_symlink(ec2)) type = "link";
                names.push_back(std::format("{} {}", type, e.path().filename().string()));
            }
            std::ranges::sort(names);
            for (const auto& n : names) ss << n << '\n';
            return ss.str();
        }
        if (name == "run_command") {
            std::string cmd = get_str("command");
            if (cmd.empty()) return "[tool error: 'command' required]";
            int timeout = std::clamp(get_int("timeout", 60), 1, 300);
            ShellResult r = run_shell(cmd, root, timeout);
            std::ostringstream ss;
            ss << r.output;
            if (ss.str().empty()) ss << "(no output)\n";
            ss << std::format("\n[exit_code={}{}]", r.exit_code,
                              r.timed_out ? ", timed_out" : "");
            return truncate(ss.str());
        }
        if (name == "finish") {
            return get_str("summary");
        }
        return std::format("[tool error: unknown tool '{}']", name);
    } catch (const std::exception& e) {
        return std::format("[tool error: {}]", e.what());
    }
}
 
} // namespace tools
