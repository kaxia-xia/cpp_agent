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

extern char** environ;

namespace tools {

namespace fs = std::filesystem;

inline fs::path resolve_under_root(const fs::path& root, std::string_view path) {
    fs::path p(path);
    fs::path full = p.is_absolute() ? p : (root / p);
    fs::path canon = fs::weakly_canonical(full);
    fs::path root_canon = fs::weakly_canonical(root);
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
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

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

// ── Tool schema definitions ──────────────────────────────────────────
// Each tool is exposed to the LLM via OpenAI function-calling format.

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

    // 1. read_file
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

    // 2. write_file
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

    // 3. list_dir
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

    // 4. run_command
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

    // 5. finish
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

    // 6. edit_file - Replace exact text in a file (like sed)
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object old_p; old_p["type"] = json::Value{"string"};
        old_p["description"] = json::Value{"Exact text to search for (must match exactly, including whitespace)."};
        props["old_text"] = json::Value{std::move(old_p)};
        json::Object new_p; new_p["type"] = json::Value{"string"};
        new_p["description"] = json::Value{"Replacement text."};
        props["new_text"] = json::Value{std::move(new_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path", "old_text", "new_text"});
        tools.push_back(fn("edit_file",
            "Replace the first occurrence of old_text with new_text in a file. "
            "Useful for making targeted edits without rewriting the entire file. "
            "The old_text must match exactly, including indentation and line endings.",
            std::move(p)));
    }

    // 7. delete_file / remove_file
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root."};
        props["path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("delete_file",
            "Delete a file or empty directory. For non-empty directories, use run_command with rm -rf.",
            std::move(p)));
    }

    // 8. rename_file / move_file
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object src_p; src_p["type"] = json::Value{"string"};
        src_p["description"] = json::Value{"Source path relative to workspace root."};
        props["source"] = json::Value{std::move(src_p)};
        json::Object dst_p; dst_p["type"] = json::Value{"string"};
        dst_p["description"] = json::Value{"Destination path relative to workspace root."};
        props["destination"] = json::Value{std::move(dst_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"source", "destination"});
        tools.push_back(fn("rename_file",
            "Rename or move a file or directory from source to destination.",
            std::move(p)));
    }

    // 9. copy_file
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object src_p; src_p["type"] = json::Value{"string"};
        src_p["description"] = json::Value{"Source path relative to workspace root."};
        props["source"] = json::Value{std::move(src_p)};
        json::Object dst_p; dst_p["type"] = json::Value{"string"};
        dst_p["description"] = json::Value{"Destination path relative to workspace root."};
        props["destination"] = json::Value{std::move(dst_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"source", "destination"});
        tools.push_back(fn("copy_file",
            "Copy a file from source to destination. Parent dirs of destination are created.",
            std::move(p)));
    }

    // 10. append_file
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object content_p; content_p["type"] = json::Value{"string"};
        content_p["description"] = json::Value{"Content to append to the file."};
        props["content"] = json::Value{std::move(content_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path", "content"});
        tools.push_back(fn("append_file",
            "Append content to the end of a file. Creates the file if it does not exist.",
            std::move(p)));
    }

    // 11. search_text - grep inside files
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object pattern_p; pattern_p["type"] = json::Value{"string"};
        pattern_p["description"] = json::Value{"Text or regex pattern to search for."};
        props["pattern"] = json::Value{std::move(pattern_p)};
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"File or directory path (default: root). Recursively searches directories."};
        path_p["default"] = json::Value{"."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object regex_p; regex_p["type"] = json::Value{"boolean"};
        regex_p["description"] = json::Value{"If true, treat pattern as a regex. Default false (literal string search)."};
        regex_p["default"] = json::Value{false};
        props["regex"] = json::Value{std::move(regex_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"pattern"});
        tools.push_back(fn("search_text",
            "Search for a pattern in files. Returns matching file paths and line numbers. "
            "Uses grep internally. For literal strings, set regex=false.",
            std::move(p)));
    }

    // 12. find_files - find files by name pattern
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object pattern_p; pattern_p["type"] = json::Value{"string"};
        pattern_p["description"] = json::Value{"File name pattern (glob, e.g. '*.cpp', '*.hpp', 'main*')."};
        props["pattern"] = json::Value{std::move(pattern_p)};
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Directory to search in (default: root)."};
        path_p["default"] = json::Value{"."};
        props["path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"pattern"});
        tools.push_back(fn("find_files",
            "Find files matching a glob pattern. Uses 'find' command internally. "
            "Example patterns: '*.cpp', '*.{hpp,h}', 'main*', 'test_*'.",
            std::move(p)));
    }

    // 13. file_info - get file/directory metadata
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root."};
        props["path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("file_info",
            "Get metadata about a file or directory: size, type, permissions, modification time.",
            std::move(p)));
    }

    // 14. read_multiple_files - read several files at once
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object paths_p; paths_p["type"] = json::Value{"array"};
        json::Object items_p; items_p["type"] = json::Value{"string"};
        paths_p["items"] = json::Value{std::move(items_p)};
        paths_p["description"] = json::Value{"Array of file paths to read."};
        props["paths"] = json::Value{std::move(paths_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"paths"});
        tools.push_back(fn("read_multiple_files",
            "Read multiple files at once. Returns each file's content separated by headers. "
            "More efficient than calling read_file multiple times.",
            std::move(p)));
    }

    // 15. write_multiple_files - write several files at once
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object files_p; files_p["type"] = json::Value{"array"};
        json::Object item_p; item_p["type"] = json::Value{"object"};
        json::Object item_props;
        json::Object ipath_p; ipath_p["type"] = json::Value{"string"};
        ipath_p["description"] = json::Value{"File path."};
        item_props["path"] = json::Value{std::move(ipath_p)};
        json::Object icontent_p; icontent_p["type"] = json::Value{"string"};
        icontent_p["description"] = json::Value{"File content."};
        item_props["content"] = json::Value{std::move(icontent_p)};
        item_p["properties"] = json::Value{std::move(item_props)};
        item_p["required"] = json::make_array<std::string>({"path", "content"});
        files_p["items"] = json::Value{std::move(item_p)};
        files_p["description"] = json::Value{"Array of {path, content} objects."};
        props["files"] = json::Value{std::move(files_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"files"});
        tools.push_back(fn("write_multiple_files",
            "Write multiple files at once. Each entry must have 'path' and 'content'. "
            "More efficient than calling write_file multiple times.",
            std::move(p)));
    }

    return json::Value{std::move(tools)};
}

// ── Truncate helper ──────────────────────────────────────────────────

inline std::string truncate(std::string s, size_t max_bytes = 60000) {
    if (s.size() <= max_bytes) return s;
    return s.substr(0, max_bytes) + std::format("\n...[truncated, {}/{} bytes shown]",
                                               max_bytes, s.size());
}

// ── Tool dispatch ────────────────────────────────────────────────────

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
    auto get_bool = [&](std::string_view key, bool def) -> bool {
        if (args.is_object() && args.contains(key)) {
            if (args[key].is_bool()) return args[key].as_bool();
        }
        return def;
    };
    auto get_array = [&](std::string_view key) -> std::vector<json::Value> {
        if (args.is_object() && args.contains(key) && args[key].is_array())
            return args[key].as_array();
        return {};
    };

    try {
        // ── read_file ────────────────────────────────────────────────
        if (name == "read_file") {
            std::string path = get_str("path");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());
            if (!fs::is_regular_file(resolved, ec)) return std::format("[error: not a regular file: {}]", resolved.string());
            std::ifstream f(resolved, std::ios::binary);
            if (!f) return std::format("[error: cannot open: {}]", resolved.string());
            std::ostringstream ss; ss << f.rdbuf();
            return truncate(ss.str());
        }

        // ── write_file ───────────────────────────────────────────────
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

        // ── list_dir ─────────────────────────────────────────────────
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

        // ── run_command ──────────────────────────────────────────────
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

        // ── finish ───────────────────────────────────────────────────
        if (name == "finish") {
            return get_str("summary");
        }

        // ── edit_file ────────────────────────────────────────────────
        if (name == "edit_file") {
            std::string path = get_str("path");
            std::string old_text = get_str("old_text");
            std::string new_text = get_str("new_text");
            if (path.empty()) return "[tool error: 'path' required]";
            if (old_text.empty()) return "[tool error: 'old_text' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());
            std::ifstream f(resolved, std::ios::binary);
            if (!f) return std::format("[error: cannot open: {}]", resolved.string());
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();

            size_t pos = content.find(old_text);
            if (pos == std::string::npos) {
                return std::format("[error: old_text not found in file '{}']\n"
                                   "Tip: use read_file first to see the exact content, "
                                   "then match whitespace and indentation exactly.", path);
            }
            content.replace(pos, old_text.size(), new_text);
            std::ofstream of(resolved, std::ios::binary | std::ios::trunc);
            if (!of) return std::format("[error: cannot write: {}]", resolved.string());
            of << content;
            of.close();
            return std::format("[ok: replaced one occurrence in '{}']", path);
        }

        // ── delete_file ──────────────────────────────────────────────
        if (name == "delete_file") {
            std::string path = get_str("path");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: not found: {}]", resolved.string());
            if (fs::is_directory(resolved, ec) && !fs::is_empty(resolved, ec)) {
                return std::format("[error: directory '{}' is not empty. Use run_command with rm -rf to remove it.]",
                                   resolved.string());
            }
            bool removed = fs::remove(resolved, ec);
            if (!removed || ec) return std::format("[error: failed to delete '{}': {}]", path, ec.message());
            return std::format("[ok: deleted '{}']", path);
        }

        // ── rename_file ──────────────────────────────────────────────
        if (name == "rename_file") {
            std::string src = get_str("source");
            std::string dst = get_str("destination");
            if (src.empty() || dst.empty()) return "[tool error: 'source' and 'destination' required]";
            fs::path src_resolved = resolve_under_root(root, src);
            fs::path dst_resolved = resolve_under_root(root, dst);
            std::error_code ec;
            if (!fs::exists(src_resolved, ec)) return std::format("[error: source not found: {}]", src);
            fs::create_directories(dst_resolved.parent_path(), ec);
            fs::rename(src_resolved, dst_resolved, ec);
            if (ec) return std::format("[error: rename failed: {}]", ec.message());
            return std::format("[ok: renamed '{}' -> '{}']", src, dst);
        }

        // ── copy_file ────────────────────────────────────────────────
        if (name == "copy_file") {
            std::string src = get_str("source");
            std::string dst = get_str("destination");
            if (src.empty() || dst.empty()) return "[tool error: 'source' and 'destination' required]";
            fs::path src_resolved = resolve_under_root(root, src);
            fs::path dst_resolved = resolve_under_root(root, dst);
            std::error_code ec;
            if (!fs::exists(src_resolved, ec)) return std::format("[error: source not found: {}]", src);
            if (fs::is_directory(src_resolved, ec)) {
                // Copy directory recursively
                fs::create_directories(dst_resolved, ec);
                for (auto& e : fs::recursive_directory_iterator(src_resolved, ec)) {
                    auto rel = fs::relative(e.path(), src_resolved);
                    auto target = dst_resolved / rel;
                    if (e.is_directory()) fs::create_directories(target, ec);
                    else fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing, ec);
                }
                return std::format("[ok: copied directory '{}' -> '{}']", src, dst);
            } else {
                fs::create_directories(dst_resolved.parent_path(), ec);
                fs::copy_file(src_resolved, dst_resolved, fs::copy_options::overwrite_existing, ec);
                if (ec) return std::format("[error: copy failed: {}]", ec.message());
                return std::format("[ok: copied '{}' -> '{}']", src, dst);
            }
        }

        // ── append_file ──────────────────────────────────────────────
        if (name == "append_file") {
            std::string path = get_str("path");
            std::string content = get_str("content");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            fs::create_directories(resolved.parent_path());
            std::ofstream f(resolved, std::ios::binary | std::ios::app);
            if (!f) return std::format("[error: cannot open for append: {}]", resolved.string());
            f << content;
            f.close();
            return std::format("[ok: appended {} bytes to '{}']", content.size(), path);
        }

        // ── search_text ──────────────────────────────────────────────
        if (name == "search_text") {
            std::string pattern = get_str("pattern");
            std::string path = get_str("path");
            bool regex = get_bool("regex", false);
            if (pattern.empty()) return "[tool error: 'pattern' required]";
            if (path.empty()) path = ".";

            fs::path resolved = resolve_under_root(root, path);
            std::string grep_opts = regex ? "-rn" : "-rnF";
            // Escape single quotes in pattern for shell
            std::string escaped_pattern;
            for (char c : pattern) {
                if (c == '\'') escaped_pattern += "'\\''";
                else escaped_pattern.push_back(c);
            }
            std::string cmd = std::format("grep {} '{}' '{}' 2>/dev/null || true",
                                          grep_opts, escaped_pattern, resolved.string());
            ShellResult r = run_shell(cmd, root, 30);
            if (r.output.empty()) return std::format("[no matches found for '{}']", pattern);
            return truncate(r.output);
        }

        // ── find_files ───────────────────────────────────────────────
        if (name == "find_files") {
            std::string pattern = get_str("pattern");
            std::string path = get_str("path");
            if (pattern.empty()) return "[tool error: 'pattern' required]";
            if (path.empty()) path = ".";

            fs::path resolved = resolve_under_root(root, path);
            // Use find with -name for glob pattern
            std::string escaped_pattern;
            for (char c : pattern) {
                if (c == '\'') escaped_pattern += "'\\''";
                else escaped_pattern.push_back(c);
            }
            std::string cmd = std::format("find '{}' -name '{}' -type f 2>/dev/null | head -200 || true",
                                          resolved.string(), escaped_pattern);
            ShellResult r = run_shell(cmd, root, 30);
            if (r.output.empty()) return std::format("[no files matching '{}']", pattern);
            return truncate(r.output);
        }

        // ── file_info ────────────────────────────────────────────────
        if (name == "file_info") {
            std::string path = get_str("path");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: not found: {}]", resolved.string());

            std::string type_str;
            if (fs::is_directory(resolved, ec)) type_str = "directory";
            else if (fs::is_symlink(resolved, ec)) type_str = "symlink";
            else if (fs::is_regular_file(resolved, ec)) type_str = "file";
            else type_str = "other";

            auto size = fs::file_size(resolved, ec);

            // Get permissions via stat command for human-readable output
            std::string escaped_path;
            for (char c : resolved.string()) {
                if (c == '\'') escaped_path += "'\\''";
                else escaped_path.push_back(c);
            }
            std::string perm_cmd = std::format("stat -c '%A %U:%G' '{}' 2>/dev/null || true", escaped_path);
            ShellResult perm_res = run_shell(perm_cmd, root, 5);
            std::string perms = perm_res.exit_code == 0 ? perm_res.output : "(unknown)";

            std::ostringstream ss;
            ss << std::format("path: {}\n", resolved.string());
            ss << std::format("type: {}\n", type_str);
            ss << std::format("size: {} bytes\n", size);
            ss << std::format("permissions: {}", perms);
            if (!perms.empty() && perms.back() == '\n') ss.str(ss.str().substr(0, ss.str().size() - 1));
            ss << '\n';
            return ss.str();
        }

        // ── read_multiple_files ──────────────────────────────────────
        if (name == "read_multiple_files") {
            auto paths = get_array("paths");
            if (paths.empty()) return "[tool error: 'paths' array required]";
            std::ostringstream ss;
            for (size_t i = 0; i < paths.size(); ++i) {
                if (!paths[i].is_string()) {
                    ss << std::format("[error: paths[{}] is not a string]\n", i);
                    continue;
                }
                std::string p = paths[i].as_string();
                try {
                    fs::path resolved = resolve_under_root(root, p);
                    std::error_code ec;
                    if (!fs::exists(resolved, ec)) {
                        ss << std::format("===== {} (NOT FOUND) =====\n", p);
                        continue;
                    }
                    std::ifstream f(resolved, std::ios::binary);
                    if (!f) {
                        ss << std::format("===== {} (CANNOT OPEN) =====\n", p);
                        continue;
                    }
                    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    ss << std::format("===== {} ({}) =====\n", p, content.size());
                    ss << content;
                    if (!content.empty() && content.back() != '\n') ss << '\n';
                } catch (const std::exception& e) {
                    ss << std::format("===== {} (ERROR: {}) =====\n", p, e.what());
                }
            }
            return truncate(ss.str());
        }

        // ── write_multiple_files ─────────────────────────────────────
        if (name == "write_multiple_files") {
            auto files = get_array("files");
            if (files.empty()) return "[tool error: 'files' array required]";
            int ok_count = 0;
            int err_count = 0;
            std::ostringstream ss;
            for (size_t i = 0; i < files.size(); ++i) {
                if (!files[i].is_object()) {
                    ss << std::format("[error: files[{}] is not an object]\n", i);
                    ++err_count;
                    continue;
                }
                const auto& obj = files[i].as_object();
                auto it_path = obj.find("path");
                auto it_content = obj.find("content");
                if (it_path == obj.end() || !it_path->second.is_string() ||
                    it_content == obj.end() || !it_content->second.is_string()) {
                    ss << std::format("[error: files[{}] missing 'path' or 'content']\n", i);
                    ++err_count;
                    continue;
                }
                std::string p = it_path->second.as_string();
                std::string c = it_content->second.as_string();
                try {
                    fs::path resolved = resolve_under_root(root, p);
                    fs::create_directories(resolved.parent_path());
                    std::ofstream f(resolved, std::ios::binary | std::ios::trunc);
                    if (!f) {
                        ss << std::format("[error: cannot write '{}']\n", p);
                        ++err_count;
                        continue;
                    }
                    f << c;
                    f.close();
                    ss << std::format("[ok: wrote {} bytes to '{}']\n", c.size(), p);
                    ++ok_count;
                } catch (const std::exception& e) {
                    ss << std::format("[error: '{}': {}]\n", p, e.what());
                    ++err_count;
                }
            }
            ss << std::format("[summary: {} ok, {} errors]", ok_count, err_count);
            return ss.str();
        }

        return std::format("[tool error: unknown tool '{}']", name);
    } catch (const std::exception& e) {
        return std::format("[tool error: {}]", e.what());
    }
}

} // namespace tools
