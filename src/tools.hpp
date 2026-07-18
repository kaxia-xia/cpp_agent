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

    // 1. read_file - supports large files with offset/limit or line range
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root (or absolute)."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object offset_p; offset_p["type"] = json::Value{"integer"};
        offset_p["description"] = json::Value{"Byte offset to start reading from (0 = beginning). Useful for large files. Ignored if start_line is set."};
        offset_p["default"] = json::Value{0};
        props["offset"] = json::Value{std::move(offset_p)};
        json::Object limit_p; limit_p["type"] = json::Value{"integer"};
        limit_p["description"] = json::Value{"Maximum number of bytes to read. Omit or set to 0 to read until end (capped at max_bytes). Ignored if start_line/end_line is set."};
        limit_p["default"] = json::Value{0};
        props["limit"] = json::Value{std::move(limit_p)};
        json::Object maxb_p; maxb_p["type"] = json::Value{"integer"};
        maxb_p["description"] = json::Value{"Maximum total bytes to return (default 200000). Increase for very large files."};
        maxb_p["default"] = json::Value{200000};
        props["max_bytes"] = json::Value{std::move(maxb_p)};
        json::Object start_line_p; start_line_p["type"] = json::Value{"integer"};
        start_line_p["description"] = json::Value{"Start line number (1-based). Reads from this line onward. Overrides offset/limit."};
        start_line_p["default"] = json::Value{0};
        props["start_line"] = json::Value{std::move(start_line_p)};
        json::Object end_line_p; end_line_p["type"] = json::Value{"integer"};
        end_line_p["description"] = json::Value{"End line number (1-based, inclusive). Only used when start_line is set. 0 means read to end of file."};
        end_line_p["default"] = json::Value{0};
        props["end_line"] = json::Value{std::move(end_line_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("read_file",
            "Read file contents with optional byte offset/limit for large files, "
            "or line-based reading with start_line/end_line. "
            "For small files just pass 'path'. For large files use offset/limit "
            "to read in chunks, or use start_line/end_line to read by line numbers.",
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

    // 6. edit_file - Replace exact text in a file (like sed), optionally at a specific line
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object old_p; old_p["type"] = json::Value{"string"};
        old_p["description"] = json::Value{"Exact text to search for (must match exactly, including whitespace). Ignored if line is set."};
        props["old_text"] = json::Value{std::move(old_p)};
        json::Object new_p; new_p["type"] = json::Value{"string"};
        new_p["description"] = json::Value{"Replacement text."};
        props["new_text"] = json::Value{std::move(new_p)};
        json::Object line_p; line_p["type"] = json::Value{"integer"};
        line_p["description"] = json::Value{"Line number (1-based) to replace. When set, replaces the entire line content with new_text, ignoring old_text."};
        line_p["default"] = json::Value{0};
        props["line"] = json::Value{std::move(line_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path", "new_text"});
        tools.push_back(fn("edit_file",
            "Replace the first occurrence of old_text with new_text in a file, "
            "or replace an entire line by number. "
            "Useful for making targeted edits without rewriting the entire file. "
            "The old_text must match exactly, including indentation and line endings. "
            "Use 'line' parameter to replace a specific line by number.",
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
        json::Object maxb_p; maxb_p["type"] = json::Value{"integer"};
        maxb_p["description"] = json::Value{"Max bytes of output (default 200000)."};
        maxb_p["default"] = json::Value{200000};
        props["max_bytes"] = json::Value{std::move(maxb_p)};
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
        json::Object start_line_p; start_line_p["type"] = json::Value{"integer"};
        start_line_p["description"] = json::Value{"Start line number (1-based). Applies to all files. Overrides max_bytes."};
        start_line_p["default"] = json::Value{0};
        props["start_line"] = json::Value{std::move(start_line_p)};
        json::Object end_line_p; end_line_p["type"] = json::Value{"integer"};
        end_line_p["description"] = json::Value{"End line number (1-based, inclusive). Only used when start_line is set. 0 means read to end."};
        end_line_p["default"] = json::Value{0};
        props["end_line"] = json::Value{std::move(end_line_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"paths"});
        tools.push_back(fn("read_multiple_files",
            "Read multiple files at once. Returns each file's content separated by headers. "
            "More efficient than calling read_file multiple times. "
            "Supports start_line/end_line for line-based reading.",
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


    // 16. fetch_url - HTTP GET a URL and return the body
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object url_p; url_p["type"] = json::Value{"string"};
        url_p["description"] = json::Value{"URL to fetch (http:// or https://)."};
        props["url"] = json::Value{std::move(url_p)};
        json::Object to_p; to_p["type"] = json::Value{"integer"};
        to_p["description"] = json::Value{"Timeout in seconds (default 30)."};
        to_p["default"] = json::Value{30};
        props["timeout"] = json::Value{std::move(to_p)};
        json::Object maxb_p; maxb_p["type"] = json::Value{"integer"};
        maxb_p["description"] = json::Value{"Max bytes to return (default 200000)."};
        maxb_p["default"] = json::Value{200000};
        props["max_bytes"] = json::Value{std::move(maxb_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"url"});
        tools.push_back(fn("fetch_url",
            "Fetch a URL via HTTP GET and return the response body. "
            "Useful for reading web pages, APIs, raw text from the internet. "
            "Returns status code and body content.",
            std::move(p)));
    }

    // 17. parse_html - parse HTML and extract text or specific elements
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object html_p; html_p["type"] = json::Value{"string"};
        html_p["description"] = json::Value{"HTML content to parse."};
        props["html"] = json::Value{std::move(html_p)};
        json::Object query_p; query_p["type"] = json::Value{"string"};
        query_p["description"] = json::Value{"CSS selector or 'text' to extract all text, or 'links' to extract all links."};
        query_p["default"] = json::Value{"text"};
        props["query"] = json::Value{std::move(query_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"html"});
        tools.push_back(fn("parse_html",
            "Parse HTML content and extract information. "
            "Use query='text' to get all visible text, query='links' to get all links, "
            "or a CSS selector like 'h1', '.class', '#id', 'div p' to extract specific elements. "
            "Uses python3 with html.parser internally.",
            std::move(p)));
    }

    // 18. parse_xml - parse XML and extract text or specific elements
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object xml_p; xml_p["type"] = json::Value{"string"};
        xml_p["description"] = json::Value{"XML content to parse."};
        props["xml"] = json::Value{std::move(xml_p)};
        json::Object xpath_p; xpath_p["type"] = json::Value{"string"};
        xpath_p["description"] = json::Value{"XPath expression or 'text' to extract all text, or 'structure' to show the XML tree structure."};
        xpath_p["default"] = json::Value{"text"};
        props["xpath"] = json::Value{std::move(xpath_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"xml"});
        tools.push_back(fn("parse_xml",
            "Parse XML content and extract information. "
            "Uses python3 with xml.etree.ElementTree internally. "
            "Supports basic XPath expressions.",
            std::move(p)));
    }

    // 19. parse_json - parse and query JSON content
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object json_p; json_p["type"] = json::Value{"string"};
        json_p["description"] = json::Value{"JSON content to parse."};
        props["json"] = json::Value{std::move(json_p)};
        json::Object query_p; query_p["type"] = json::Value{"string"};
        query_p["description"] = json::Value{"Dot-separated path to extract (e.g. 'data.items[0].name'), or empty to pretty-print the whole JSON."};
        query_p["default"] = json::Value{""};
        props["query"] = json::Value{std::move(query_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"json"});
        tools.push_back(fn("parse_json",
            "Parse and query JSON content. "
            "Use query='' to pretty-print the entire JSON. "
            "Use dot-separated paths like 'store.book[0].title' to extract specific values. "
            "Supports array indexing with [n].",
            std::move(p)));
    }

    // 20. render_mermaid - convert Mermaid diagram definition to SVG image file
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object mermaid_p; mermaid_p["type"] = json::Value{"string"};
        mermaid_p["description"] = json::Value{"Mermaid diagram definition (the content after the ```mermaid block)."};
        props["mermaid"] = json::Value{std::move(mermaid_p)};
        json::Object output_p; output_p["type"] = json::Value{"string"};
        output_p["description"] = json::Value{"Output SVG file path (relative to workspace root)."};
        props["output"] = json::Value{std::move(output_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"mermaid", "output"});
        tools.push_back(fn("render_mermaid",
            "Convert a Mermaid diagram definition to an SVG image file. "
            "Requires mmdc (mermaid-cli) to be installed. "
            "If mmdc is not available, falls back to generating a text-based diagram description.",
            std::move(p)));
    }

    // 21. image_info - get metadata about an image file
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path to image file (relative to workspace root)."};
        props["path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("image_info",
            "Get metadata about an image file: format, dimensions, color mode, file size. "
            "Supports PNG, JPEG, GIF, BMP, WebP and other common formats. "
            "Uses python3 with PIL (Pillow) internally.",
            std::move(p)));
    }

    // 22. image_convert - convert image between formats or resize
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object src_p; src_p["type"] = json::Value{"string"};
        src_p["description"] = json::Value{"Source image path."};
        props["source"] = json::Value{std::move(src_p)};
        json::Object dst_p; dst_p["type"] = json::Value{"string"};
        dst_p["description"] = json::Value{"Destination image path (extension determines format)."};
        props["destination"] = json::Value{std::move(dst_p)};
        json::Object width_p; width_p["type"] = json::Value{"integer"};
        width_p["description"] = json::Value{"Resize width (optional, keeps aspect ratio if only width or height given)."};
        width_p["default"] = json::Value{0};
        props["width"] = json::Value{std::move(width_p)};
        json::Object height_p; height_p["type"] = json::Value{"integer"};
        height_p["description"] = json::Value{"Resize height (optional)."};
        height_p["default"] = json::Value{0};
        props["height"] = json::Value{std::move(height_p)};
        json::Object quality_p; quality_p["type"] = json::Value{"integer"};
        quality_p["description"] = json::Value{"Output quality 1-100 (default 85, for JPEG/WebP)."};
        quality_p["default"] = json::Value{85};
        props["quality"] = json::Value{std::move(quality_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"source", "destination"});
        tools.push_back(fn("image_convert",
            "Convert an image between formats or resize it. "
            "Output format is determined by the destination file extension "
            "(e.g. .png, .jpg, .gif, .bmp, .webp). "
            "Uses python3 with PIL (Pillow) internally.",
            std::move(p)));
    }

    // 23. image_to_svg - convert bitmap image to SVG (trace/vectorize)
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object src_p; src_p["type"] = json::Value{"string"};
        src_p["description"] = json::Value{"Source image path (PNG, JPEG, etc.)."};
        props["source"] = json::Value{std::move(src_p)};
        json::Object dst_p; dst_p["type"] = json::Value{"string"};
        dst_p["description"] = json::Value{"Output SVG file path."};
        props["destination"] = json::Value{std::move(dst_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"source", "destination"});
        tools.push_back(fn("image_to_svg",
            "Convert a bitmap image to SVG format by embedding it as a base64 data URI. "
            "This creates a valid SVG that displays the original image. "
            "For true vectorization, use run_command with 'potrace' or online tools. "
            "Uses python3 with PIL internally.",
            std::move(p)));
    }

    // ── 24. clipboard - read/write system clipboard ──────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object action_p; action_p["type"] = json::Value{"string"};
        action_p["description"] = json::Value{"Action: 'get' to read clipboard, 'set' to write clipboard."};
        action_p["enum"] = json::make_array<std::string>({"get", "set"});
        props["action"] = json::Value{std::move(action_p)};
        json::Object content_p; content_p["type"] = json::Value{"string"};
        content_p["description"] = json::Value{"Content to write (required when action='set')."};
        content_p["default"] = json::Value{""};
        props["content"] = json::Value{std::move(content_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"action"});
        tools.push_back(fn("clipboard",
            "Read or write the Android system clipboard. "
            "Use action='get' to retrieve clipboard text, action='set' with 'content' to set clipboard text. "
            "Uses termux-clipboard-get / termux-clipboard-set.",
            std::move(p)));
    }

    // ── 25. notify - send Android notification ───────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object title_p; title_p["type"] = json::Value{"string"};
        title_p["description"] = json::Value{"Notification title."};
        props["title"] = json::Value{std::move(title_p)};
        json::Object content_p; content_p["type"] = json::Value{"string"};
        content_p["description"] = json::Value{"Notification content text."};
        props["content"] = json::Value{std::move(content_p)};
        json::Object priority_p; priority_p["type"] = json::Value{"string"};
        priority_p["description"] = json::Value{"Priority: 'low', 'normal', 'high' (default: 'normal')."};
        priority_p["default"] = json::Value{"normal"};
        props["priority"] = json::Value{std::move(priority_p)};
        json::Object alert_p; alert_p["type"] = json::Value{"boolean"};
        alert_p["description"] = json::Value{"Whether to alert (sound/vibrate). Default true."};
        alert_p["default"] = json::Value{true};
        props["alert_once"] = json::Value{std::move(alert_p)};
        json::Object sound_p; sound_p["type"] = json::Value{"boolean"};
        sound_p["description"] = json::Value{"Whether to play a notification sound. Default true."};
        sound_p["default"] = json::Value{true};
        props["sound"] = json::Value{std::move(sound_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"title", "content"});
        tools.push_back(fn("notify",
            "Send a notification to the Android notification bar. "
            "Uses termux-notification. Great for alerting when long tasks complete.",
            std::move(p)));
    }

    // ── 26. vibrate - haptic feedback ────────────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object dur_p; dur_p["type"] = json::Value{"integer"};
        dur_p["description"] = json::Value{"Vibration duration in milliseconds (default: 200)."};
        dur_p["default"] = json::Value{200};
        props["duration_ms"] = json::Value{std::move(dur_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("vibrate",
            "Make the device vibrate for a specified duration. "
            "Uses termux-vibrate. Fun for feedback or alerts.",
            std::move(p)));
    }

    // ── 28. run_python - execute Python code ─────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object code_p; code_p["type"] = json::Value{"string"};
        code_p["description"] = json::Value{"Python code to execute."};
        props["code"] = json::Value{std::move(code_p)};
        json::Object to_p; to_p["type"] = json::Value{"integer"};
        to_p["description"] = json::Value{"Timeout in seconds (default: 30)."};
        to_p["default"] = json::Value{30};
        props["timeout"] = json::Value{std::move(to_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"code"});
        tools.push_back(fn("run_python",
            "Execute a Python code snippet and return stdout+stderr. "
            "Useful for quick calculations, data processing, text manipulation, "
            "or any task that benefits from Python's standard library. "
            "Runs with python3.",
            std::move(p)));
    }

    // ── 29. ocr - optical character recognition ──────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path to image file to perform OCR on."};
        props["image_path"] = json::Value{std::move(path_p)};
        json::Object lang_p; lang_p["type"] = json::Value{"string"};
        lang_p["description"] = json::Value{"Language(s) for OCR, e.g. 'eng', 'chi_sim', 'eng+chi_sim'. Default: 'eng'."};
        lang_p["default"] = json::Value{"eng"};
        props["lang"] = json::Value{std::move(lang_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"image_path"});
        tools.push_back(fn("ocr",
            "Perform optical character recognition (OCR) on an image file. "
            "Extracts text from images using Tesseract OCR engine. "
            "Supports multiple languages (e.g. 'eng', 'chi_sim', 'eng+chi_sim'). "
            "Requires tesseract to be installed.",
            std::move(p)));
    }

    // ── 30. qr_encode - generate QR code ─────────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object data_p; data_p["type"] = json::Value{"string"};
        data_p["description"] = json::Value{"Data to encode in the QR code (text, URL, etc.)."};
        props["data"] = json::Value{std::move(data_p)};
        json::Object out_p; out_p["type"] = json::Value{"string"};
        out_p["description"] = json::Value{"Output image file path (e.g. 'qrcode.png')."};
        props["output"] = json::Value{std::move(out_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"data", "output"});
        tools.push_back(fn("qr_encode",
            "Generate a QR code image from text or URL data. "
            "Uses Python qrcode library. Outputs a PNG image file.",
            std::move(p)));
    }

    // ── 31. qr_decode - decode QR code / barcode from image ─────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path to image file containing a QR code or barcode."};
        props["image_path"] = json::Value{std::move(path_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"image_path"});
        tools.push_back(fn("qr_decode",
            "Decode a QR code or barcode from an image file. "
            "Uses Python pyzbar library (wraps zbar). "
            "Returns the decoded data content.",
            std::move(p)));
    }

    // ── 32. diff_files - compare two files ───────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object f1_p; f1_p["type"] = json::Value{"string"};
        f1_p["description"] = json::Value{"First file path."};
        props["file1"] = json::Value{std::move(f1_p)};
        json::Object f2_p; f2_p["type"] = json::Value{"string"};
        f2_p["description"] = json::Value{"Second file path."};
        props["file2"] = json::Value{std::move(f2_p)};
        json::Object ctx_p; ctx_p["type"] = json::Value{"integer"};
        ctx_p["description"] = json::Value{"Context lines around changes (default: 3)."};
        ctx_p["default"] = json::Value{3};
        props["context_lines"] = json::Value{std::move(ctx_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"file1", "file2"});
        tools.push_back(fn("diff_files",
            "Compare two files and show the differences (unified diff format). "
            "Uses 'diff -u' internally. Useful for seeing what changed between versions.",
            std::move(p)));
    }

    // ── 33. compress - create archive ────────────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object src_p; src_p["type"] = json::Value{"string"};
        src_p["description"] = json::Value{"Source file or directory to compress."};
        props["source"] = json::Value{std::move(src_p)};
        json::Object dst_p; dst_p["type"] = json::Value{"string"};
        dst_p["description"] = json::Value{"Output archive path (e.g. 'archive.zip', 'archive.tar.gz')."};
        props["output"] = json::Value{std::move(dst_p)};
        json::Object fmt_p; fmt_p["type"] = json::Value{"string"};
        fmt_p["description"] = json::Value{"Archive format: 'zip' or 'tar.gz' (default: auto from output extension)."};
        fmt_p["default"] = json::Value{""};
        props["format"] = json::Value{std::move(fmt_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"source", "output"});
        tools.push_back(fn("compress",
            "Create a compressed archive (zip or tar.gz) from a file or directory. "
            "Uses zip or tar+gzip internally.",
            std::move(p)));
    }

    // ── 34. decompress - extract archive ─────────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object arc_p; arc_p["type"] = json::Value{"string"};
        arc_p["description"] = json::Value{"Archive file path to extract (zip, tar.gz, tar.bz2, etc.)."};
        props["archive"] = json::Value{std::move(arc_p)};
        json::Object dst_p; dst_p["type"] = json::Value{"string"};
        dst_p["description"] = json::Value{"Output directory to extract into (default: same name as archive without extension)."};
        dst_p["default"] = json::Value{""};
        props["output_dir"] = json::Value{std::move(dst_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"archive"});
        tools.push_back(fn("decompress",
            "Extract a compressed archive (zip, tar.gz, tar.bz2, tar.xz, etc.). "
            "Auto-detects format from file extension. Uses unzip or tar internally.",
            std::move(p)));
    }

    // ── 35. system_info - get device/system information ──────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object cat_p; cat_p["type"] = json::Value{"string"};
        cat_p["description"] = json::Value{"Category: 'all', 'battery', 'cpu', 'memory', 'storage', 'network'. Default: 'all'."};
        cat_p["default"] = json::Value{"all"};
        props["category"] = json::Value{std::move(cat_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("system_info",
            "Get information about the Android device: battery status, CPU info, "
            "memory usage, storage, network interfaces. "
            "Uses termux-battery-status, /proc filesystem, and other system commands.",
            std::move(p)));
    }

    // ── 36. weather - query weather ──────────────────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object loc_p; loc_p["type"] = json::Value{"string"};
        loc_p["description"] = json::Value{"Location (city name or coordinates). Default: auto-detect from IP."};
        loc_p["default"] = json::Value{""};
        props["location"] = json::Value{std::move(loc_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("weather",
            "Query current weather and forecast for a location. "
            "Uses wttr.in API via curl. Returns temperature, conditions, humidity, wind, etc. "
            "If no location given, auto-detects from IP address.",
            std::move(p)));
    }

    // ── 37. get_location - get current geographic location ────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object prov_p; prov_p["type"] = json::Value{"string"};
        prov_p["description"] = json::Value{"Provider: 'network' (fast, coarse), 'gps' (slow, precise). Default: 'network'."};
        prov_p["default"] = json::Value{"network"};
        props["provider"] = json::Value{std::move(prov_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("get_location",
            "Get the current geographic location (latitude, longitude) of the device. "
            "Uses termux-location. Provider 'network' returns quickly with ~30m accuracy; "
            "'gps' takes longer but is more precise. Returns latitude, longitude, accuracy, and provider info.",
            std::move(p)));
    }

    // ── 38. get_datetime - get current date and time ──────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object fmt_p; fmt_p["type"] = json::Value{"string"};
        fmt_p["description"] = json::Value{"Format string (strftime format, e.g. '%Y-%m-%d %H:%M:%S', '%A', '%Y-%m-%d'). Default: '%Y-%m-%d %H:%M:%S %Z'."};
        fmt_p["default"] = json::Value{"%Y-%m-%d %H:%M:%S %Z"};
        props["format"] = json::Value{std::move(fmt_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("get_datetime",
            "Get the current date and time on the device. "
            "Returns the current system date/time in the specified format. "
            "Uses strftime formatting: %Y=year, %m=month, %d=day, %H=hour, %M=minute, %S=second, "
            "%A=weekday name, %B=month name, %Z=timezone. "
            "Default format: '%Y-%m-%d %H:%M:%S %Z'.",
            std::move(p)));
    }

    // ── 37. screenshot - capture device screen ───────────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object out_p; out_p["type"] = json::Value{"string"};
        out_p["description"] = json::Value{"Output PNG file path (default: 'screenshot.png')."};
        out_p["default"] = json::Value{"screenshot.png"};
        props["output"] = json::Value{std::move(out_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({});
        tools.push_back(fn("screenshot",
            "Capture a screenshot of the device screen. "
            "Uses termux-screencap. The agent can 'see' what's on your screen! "
            "Combine with image_info or ocr for screen analysis.",
            std::move(p)));
    }

    // ── 38. plot_chart - generate data visualization ─────────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object type_p; type_p["type"] = json::Value{"string"};
        type_p["description"] = json::Value{"Chart type: 'bar', 'line', 'pie', 'scatter'."};
        props["chart_type"] = json::Value{std::move(type_p)};
        json::Object data_p; data_p["type"] = json::Value{"string"};
        data_p["description"] = json::Value{"Data in JSON format. For bar/line: {\"labels\":[\"A\",\"B\"],\"values\":[10,20]}. For pie: same. For scatter: {\"x\":[1,2,3],\"y\":[4,5,6]}."};
        props["data_json"] = json::Value{std::move(data_p)};
        json::Object title_p; title_p["type"] = json::Value{"string"};
        title_p["description"] = json::Value{"Chart title (optional)."};
        title_p["default"] = json::Value{""};
        props["title"] = json::Value{std::move(title_p)};
        json::Object out_p; out_p["type"] = json::Value{"string"};
        out_p["description"] = json::Value{"Output image file path (e.g. 'chart.png'). Default: 'chart.png'."};
        out_p["default"] = json::Value{"chart.png"};
        props["output"] = json::Value{std::move(out_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"chart_type", "data_json"});
        tools.push_back(fn("plot_chart",
            "Generate a chart/plot image from data. Supports bar, line, pie, and scatter charts. "
            "Uses Python matplotlib. Returns the path to the generated image.",
            std::move(p)));
    }

    // ── 39. show_image - display an image file in Termux ──────────────
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path to the image file to display (PNG, JPEG, SVG, etc.)."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object method_p; method_p["type"] = json::Value{"string"};
        method_p["description"] = json::Value{"Display method: 'auto' (try best available), 'termux-open' (open with system viewer), 'ascii' (render as ASCII art in terminal). Default: 'auto'."};
        method_p["default"] = json::Value{"auto"};
        method_p["enum"] = json::make_array<std::string>({"auto", "termux-open", "ascii"});
        props["method"] = json::Value{std::move(method_p)};
        json::Object width_p; width_p["type"] = json::Value{"integer"};
        width_p["description"] = json::Value{"ASCII render width in characters (default: terminal width, min 20, max 200). Only used when method='ascii'."};
        width_p["default"] = json::Value{0};
        props["ascii_width"] = json::Value{std::move(width_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("show_image",
            "Display an image file (PNG, JPEG, SVG, etc.) in the Termux terminal. "
            "Three methods available: "
            "'termux-open' opens the image with the system image viewer (best for photos); "
            "'ascii' renders the image as ASCII art directly in the terminal (no external viewer needed); "
            "'auto' tries 'termux-open' first, falls back to 'ascii' if unavailable. "
            "For SVG files, converts to PNG first before displaying. "
            "Useful for viewing screenshots, charts, diagrams, or any image file.",
            std::move(p)));
    }

    return json::Value{std::move(tools)};
}

// ── Truncate helper ──────────────────────────────────────────────────

inline std::string truncate(std::string s, size_t max_bytes = 200000) {
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

            auto file_size = fs::file_size(resolved, ec);
            int start_line = get_int("start_line", 0);
            int end_line = get_int("end_line", 0);

            // Line-based reading mode (start_line > 0)
            if (start_line > 0) {
                std::ifstream f(resolved);
                if (!f) return std::format("[error: cannot open: {}]", resolved.string());

                std::vector<std::string> all_lines;
                std::string line;
                while (std::getline(f, line)) {
                    all_lines.push_back(line);
                }
                size_t total_lines = all_lines.size();

                if (start_line > static_cast<int>(total_lines)) {
                    return std::format("[error: start_line {} exceeds file length ({} lines)]",
                                       start_line, total_lines);
                }

                size_t from = static_cast<size_t>(start_line - 1);  // 0-based
                size_t to = (end_line > 0) ? static_cast<size_t>(end_line) : total_lines;
                if (to > total_lines) to = total_lines;

                size_t max_bytes = static_cast<size_t>(std::max(1024, get_int("max_bytes", 200000)));
                std::ostringstream ss;
                size_t bytes_written = 0;
                bool truncated = false;

                for (size_t i = from; i < to; ++i) {
                    std::string ln = all_lines[i] + '\n';
                    if (bytes_written + ln.size() > max_bytes) {
                        truncated = true;
                        break;
                    }
                    ss << ln;
                    bytes_written += ln.size();
                }

                if (truncated) {
                    ss << std::format("[... truncated at {} bytes, showing lines {}-{} of {}]\n",
                                      max_bytes, start_line,
                                      static_cast<int>(from + (to - from)), total_lines);
                } else {
                    ss << std::format("[showing lines {}-{} of {}]\n",
                                      start_line,
                                      (end_line > 0 && end_line <= static_cast<int>(total_lines)) ? end_line : static_cast<int>(total_lines),
                                      total_lines);
                }
                return ss.str();
            }

            // Byte-based reading mode (original)
            size_t offset = static_cast<size_t>(std::max(0, get_int("offset", 0)));
            size_t limit = static_cast<size_t>(std::max(0, get_int("limit", 0)));
            size_t max_bytes = static_cast<size_t>(std::max(1024, get_int("max_bytes", 200000)));

            // Clamp offset to file size
            if (offset > static_cast<size_t>(file_size)) {
                return std::format("[ok: offset {} is beyond file size {}, nothing to read]", offset, file_size);
            }

            // If limit is 0, read until end (capped by max_bytes)
            size_t to_read = (limit == 0) ? (static_cast<size_t>(file_size) - offset) : limit;
            to_read = std::min(to_read, max_bytes);

            std::ifstream f(resolved, std::ios::binary);
            if (!f) return std::format("[error: cannot open: {}]", resolved.string());

            f.seekg(static_cast<std::streamoff>(offset));
            std::string buf(to_read, '\0');
            f.read(buf.data(), static_cast<std::streamsize>(to_read));
            size_t bytes_read = static_cast<size_t>(f.gcount());
            buf.resize(bytes_read);

            std::ostringstream ss;
            ss << buf;
            size_t remaining = static_cast<size_t>(file_size) - offset - bytes_read;

            if (remaining > 0) {
                ss << std::format("\n...[showing bytes {}-{} of {} ({} more available)]",
                                  offset, offset + bytes_read - 1, file_size, remaining);
            } else if (offset > 0) {
                ss << std::format("\n[showing bytes {}-{} of {}]",
                                  offset, offset + bytes_read - 1, file_size);
            }
            return ss.str();
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
            int line = get_int("line", 0);
            if (path.empty()) return "[tool error: 'path' required]";
            if (new_text.empty() && line == 0) return "[tool error: 'new_text' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());

            // Line-based edit mode
            if (line > 0) {
                std::ifstream f(resolved);
                if (!f) return std::format("[error: cannot open: {}]", resolved.string());
                std::vector<std::string> all_lines;
                std::string l;
                while (std::getline(f, l)) {
                    all_lines.push_back(l);
                }
                f.close();

                if (line > static_cast<int>(all_lines.size())) {
                    return std::format("[error: line {} exceeds file length ({} lines)]",
                                       line, all_lines.size());
                }

                all_lines[static_cast<size_t>(line - 1)] = new_text;

                std::ofstream of(resolved, std::ios::binary | std::ios::trunc);
                if (!of) return std::format("[error: cannot write: {}]", resolved.string());
                for (size_t i = 0; i < all_lines.size(); ++i) {
                    of << all_lines[i];
                    if (i + 1 < all_lines.size()) of << '\n';
                }
                of.close();
                return std::format("[ok: replaced line {} in '{}']", line, path);
            }

            // Text-based edit mode (original)
            if (old_text.empty()) return "[tool error: 'old_text' required when 'line' is not set]";
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
            size_t max_bytes = static_cast<size_t>(std::max(1024, get_int("max_bytes", 200000)));
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
            return truncate(r.output, max_bytes);
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
            size_t max_bytes_per_file = static_cast<size_t>(std::max(1024, get_int("max_bytes", 100000)));
            int start_line = get_int("start_line", 0);
            int end_line = get_int("end_line", 0);
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

                    // Line-based reading mode
                    if (start_line > 0) {
                        std::ifstream f(resolved);
                        if (!f) {
                            ss << std::format("===== {} (CANNOT OPEN) =====\n", p);
                            continue;
                        }
                        std::vector<std::string> all_lines;
                        std::string line;
                        while (std::getline(f, line)) {
                            all_lines.push_back(line);
                        }
                        size_t total_lines = all_lines.size();

                        if (start_line > static_cast<int>(total_lines)) {
                            ss << std::format("===== {} (start_line {} > {} lines) =====\n", p, start_line, total_lines);
                            continue;
                        }

                        size_t from = static_cast<size_t>(start_line - 1);
                        size_t to = (end_line > 0) ? static_cast<size_t>(end_line) : total_lines;
                        if (to > total_lines) to = total_lines;

                        ss << std::format("===== {} (lines {}-{}/{}) =====\n", p, start_line, to, total_lines);
                        for (size_t j = from; j < to; ++j) {
                            ss << all_lines[j] << '\n';
                        }
                        continue;
                    }

                    // Byte-based reading mode (original)
                    auto fsize = fs::file_size(resolved, ec);
                    std::ifstream f(resolved, std::ios::binary);
                    if (!f) {
                        ss << std::format("===== {} (CANNOT OPEN) =====\n", p);
                        continue;
                    }
                    size_t to_read = std::min(static_cast<size_t>(fsize), max_bytes_per_file);
                    std::string buf(to_read, '\0');
                    f.read(buf.data(), static_cast<std::streamsize>(to_read));
                    size_t bytes_read = static_cast<size_t>(f.gcount());
                    buf.resize(bytes_read);

                    ss << std::format("===== {} ({}/{}) =====\n", p, bytes_read, fsize);
                    ss << buf;
                    if (!buf.empty() && buf.back() != '\n') ss << '\n';
                    if (static_cast<size_t>(fsize) > bytes_read) {
                        ss << std::format("[... truncated, showing first {} of {} bytes]\n", bytes_read, fsize);
                    }
                } catch (const std::exception& e) {
                    ss << std::format("===== {} (ERROR: {}) =====\n", p, e.what());
                }
            }
            return ss.str();
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


        // ── fetch_url ────────────────────────────────────────────────
        if (name == "fetch_url") {
            std::string url = get_str("url");
            if (url.empty()) return "[tool error: 'url' required]";
            int timeout = std::clamp(get_int("timeout", 30), 5, 120);
            size_t max_bytes = static_cast<size_t>(std::max(1024, get_int("max_bytes", 200000)));

            std::string escaped_url;
            for (char c : url) {
                if (c == '\'') escaped_url += "'\\''";
                else escaped_url.push_back(c);
            }
            std::string cmd = std::format("curl -sL --max-time {} '{}' 2>/dev/null || true", timeout, escaped_url);
            ShellResult r = run_shell(cmd, root, timeout + 10);

            std::string status_cmd = "curl -sL -o /dev/null -w '%{http_code}' --max-time " + std::to_string(timeout) + " '" + escaped_url + "' 2>/dev/null || echo '000'";
            ShellResult status_r = run_shell(status_cmd, root, timeout + 10);
            std::string status_code = status_r.output;
            while (!status_code.empty() && (status_code.back() == '\n' || status_code.back() == '\r'))
                status_code.pop_back();

            std::string result = r.output;
            size_t total = result.size();
            if (result.size() > max_bytes) {
                result = result.substr(0, max_bytes);
            }

            std::ostringstream ss;
            ss << std::format("[HTTP {} | {} bytes", status_code, total);
            if (total > max_bytes) ss << std::format(" (showing first {})", max_bytes);
            ss << "]\n";
            ss << result;
            return ss.str();
        }

        // ── parse_html ───────────────────────────────────────────────
        if (name == "parse_html") {
            std::string html = get_str("html");
            std::string query = get_str("query");
            if (html.empty()) return "[tool error: 'html' required]";
            if (query.empty()) query = "text";

            std::string py_script;
            if (query == "text") {
                py_script = "import html.parser, sys\n"
                    "class T(html.parser.HTMLParser):\n"
                    "  def __init__(self):\n"
                    "    super().__init__()\n"
                    "    self.text = []\n"
                    "    self.skip = False\n"
                    "  def handle_starttag(self, tag, attrs):\n"
                    "    if tag in ('script','style'): self.skip = True\n"
                    "  def handle_endtag(self, tag):\n"
                    "    if tag in ('script','style'): self.skip = False\n"
                    "  def handle_data(self, data):\n"
                    "    if not self.skip:\n"
                    "      data = data.strip()\n"
                    "      if data: self.text.append(data)\n"
                    "p = T()\n"
                    "p.feed(sys.stdin.read())\n"
                    "print('\\n'.join(p.text))\n";
            } else if (query == "links") {
                py_script = "import html.parser, sys\n"
                    "class L(html.parser.HTMLParser):\n"
                    "  def __init__(self):\n"
                    "    super().__init__()\n"
                    "    self.links = []\n"
                    "  def handle_starttag(self, tag, attrs):\n"
                    "    if tag == 'a':\n"
                    "      d = dict(attrs)\n"
                    "      self.cap = d.get('href','')\n"
                    "    else: self.cap = None\n"
                    "  def handle_data(self, data):\n"
                    "    if getattr(self,'cap',None) and data.strip():\n"
                    "      self.links.append((self.cap, data.strip()))\n"
                    "p = L()\n"
                    "p.feed(sys.stdin.read())\n"
                    "for h,t in p.links:\n"
                    "  if h: print(f'{h}  ({t})' if t else h)\n";
            } else {
                py_script = "import html.parser, sys\n"
                    "q = sys.argv[1] if len(sys.argv)>1 else ''\n"
                    "class F(html.parser.HTMLParser):\n"
                    "  def __init__(self):\n"
                    "    super().__init__()\n"
                    "    self.r = []\n"
                    "    self.cap = False\n"
                    "  def handle_starttag(self, tag, attrs):\n"
                    "    self.cap = False\n"
                    "    cls = set(); iid = ''\n"
                    "    for n,v in attrs:\n"
                    "      if n=='class' and v: cls = set(v.split())\n"
                    "      if n=='id' and v: iid = v\n"
                    "    if q.startswith('.'): self.cap = q[1:] in cls\n"
                    "    elif q.startswith('#'): self.cap = q[1:] == iid\n"
                    "    elif q == tag: self.cap = True\n"
                    "    elif ' ' in q and q.split()[-1]==tag: self.cap = True\n"
                    "  def handle_data(self, data):\n"
                    "    if self.cap and data.strip(): self.r.append(data.strip())\n"
                    "p = F()\n"
                    "p.feed(sys.stdin.read())\n"
                    "for x in p.r: print(x)\n";
            }

            std::string htmlfile = (fs::temp_directory_path() / "agent_html_in.html").string();
            std::string pyfile = (fs::temp_directory_path() / "agent_html_parse.py").string();
            {
                std::ofstream pf(pyfile); pf << py_script;
            }
            {
                std::ofstream hf(htmlfile); hf << html;
            }

            std::string cmd;
            if (query == "text" || query == "links") {
                cmd = std::format("python3 '{}' < '{}' 2>&1 || true", pyfile, htmlfile);
            } else {
                std::string eq; for (char c : query) { if (c == '\'') eq += "'\\''"; else eq.push_back(c); }
                cmd = std::format("python3 '{}' '{}' < '{}' 2>&1 || true", pyfile, eq, htmlfile);
            }

            ShellResult r = run_shell(cmd, root, 30);
            if (r.output.empty()) return "[no results found]";
            return truncate(r.output, 100000);
        }

        // ── parse_xml ────────────────────────────────────────────────
        if (name == "parse_xml") {
            std::string xml = get_str("xml");
            std::string xpath = get_str("xpath");
            if (xml.empty()) return "[tool error: 'xml' required]";
            if (xpath.empty()) xpath = "text";

            std::string py_script;
            if (xpath == "text") {
                py_script = "import sys, xml.etree.ElementTree as ET\n"
                    "t = ET.parse(sys.stdin)\n"
                    "def gt(e):\n"
                    "  r = [e.text] if e.text and e.text.strip() else []\n"
                    "  for c in e:\n"
                    "    r.extend(gt(c))\n"
                    "    if c.tail and c.tail.strip(): r.append(c.tail.strip())\n"
                    "  return r\n"
                    "for x in gt(t.getroot()): print(x)\n";
            } else if (xpath == "structure") {
                py_script = "import sys, xml.etree.ElementTree as ET\n"
                    "t = ET.parse(sys.stdin)\n"
                    "def sh(e, i=0):\n"
                    "  p = '  '*i\n"
                    "  a = ' '.join(f'{k}=\"{v}\"' for k,v in e.attrib.items())\n"
                    "  s = e.tag + (f' [{a}]' if a else '')\n"
                    "  tx = (e.text or '').strip()\n"
                    "  if tx: s += f' = \"{tx[:50]}\"'\n"
                    "  print(f'{p}<{s}>')\n"
                    "  for c in e: sh(c, i+1)\n"
                    "sh(t.getroot())\n";
            } else {
                py_script = "import sys, xml.etree.ElementTree as ET\n"
                    "xp = sys.argv[1] if len(sys.argv)>1 else ''\n"
                    "t = ET.parse(sys.stdin)\n"
                    "try:\n"
                    "  for e in t.getroot().findall(xp):\n"
                    "    tx = (e.text or '').strip()\n"
                    "    if tx: print(tx)\n"
                    "    else:\n"
                    "      a = ' '.join(f'{k}=\"{v}\"' for k,v in e.attrib.items())\n"
                    "      print(f'<{e.tag}>' + (f' [{a}]' if a else ''))\n"
                    "except Exception as ex: print(f'[error: {ex}]')\n";
            }

            std::string xmlfile = (fs::temp_directory_path() / "agent_xml_in.xml").string();
            std::string pyfile = (fs::temp_directory_path() / "agent_xml_parse.py").string();
            {
                std::ofstream pf(pyfile); pf << py_script;
            }
            {
                std::ofstream xf(xmlfile); xf << xml;
            }

            std::string cmd;
            if (xpath == "text" || xpath == "structure") {
                cmd = std::format("python3 '{}' < '{}' 2>&1 || true", pyfile, xmlfile);
            } else {
                std::string ex; for (char c : xpath) { if (c == '\'') ex += "'\\''"; else ex.push_back(c); }
                cmd = std::format("python3 '{}' '{}' < '{}' 2>&1 || true", pyfile, ex, xmlfile);
            }

            ShellResult r = run_shell(cmd, root, 30);
            if (r.output.empty()) return "[no results found]";
            return truncate(r.output, 100000);
        }

        // ── parse_json ───────────────────────────────────────────────
        if (name == "parse_json") {
            std::string json_str = get_str("json");
            std::string query = get_str("query");
            if (json_str.empty()) return "[tool error: 'json' required]";

            std::string py_script = "import sys, json\n"
                "def qj(obj, path):\n"
                "  if not path: return obj\n"
                "  for p in path.split('.'):\n"
                "    if '[' in p and p.endswith(']'):\n"
                "      n, i = p[:-1].split('[')\n"
                "      idx = int(i)\n"
                "      if isinstance(obj,dict) and n in obj: obj = obj[n]\n"
                "      if isinstance(obj,list) and idx < len(obj): obj = obj[idx]\n"
                "      else: return None\n"
                "    else:\n"
                "      if isinstance(obj,dict) and p in obj: obj = obj[p]\n"
                "      else: return None\n"
                "  return obj\n"
                "d = json.load(sys.stdin)\n"
                "p = sys.argv[1] if len(sys.argv)>1 else ''\n"
                "if p:\n"
                "  r = qj(d,p)\n"
                "  if r is None: print(f'[not found: {p}]')\n"
                "  else: print(json.dumps(r, indent=2, ensure_ascii=False))\n"
                "else: print(json.dumps(d, indent=2, ensure_ascii=False))\n";

            std::string jsonfile = (fs::temp_directory_path() / "agent_json_in.json").string();
            std::string pyfile = (fs::temp_directory_path() / "agent_json_parse.py").string();
            {
                std::ofstream pf(pyfile); pf << py_script;
            }
            {
                std::ofstream jf(jsonfile); jf << json_str;
            }

            std::string cmd;
            if (query.empty()) {
                cmd = std::format("python3 '{}' '' < '{}' 2>&1 || true", pyfile, jsonfile);
            } else {
                std::string eq; for (char c : query) { if (c == '\'') eq += "'\\''"; else eq.push_back(c); }
                cmd = std::format("python3 '{}' '{}' < '{}' 2>&1 || true", pyfile, eq, jsonfile);
            }

            ShellResult r = run_shell(cmd, root, 30);
            if (r.output.empty()) return "[empty result]";
            return truncate(r.output, 100000);
        }

        // ── render_mermaid ────────────────────────────────────────────
        if (name == "render_mermaid") {
            std::string mermaid = get_str("mermaid");
            std::string output = get_str("output");
            if (mermaid.empty()) return "[tool error: 'mermaid' required]";
            if (output.empty()) return "[tool error: 'output' required]";

            fs::path output_path = resolve_under_root(root, output);
            fs::create_directories(output_path.parent_path());

            std::string check_cmd = "which mmdc 2>/dev/null || true";
            ShellResult check_r = run_shell(check_cmd, root, 5);
            bool has_mmdc = !check_r.output.empty() && check_r.output.find("mmdc") != std::string::npos;

            if (has_mmdc) {
                std::string mmdfile = (fs::temp_directory_path() / "agent_diagram.mmd").string();
                { std::ofstream mf(mmdfile); mf << mermaid; }
                std::string cmd = std::format("mmdc -i '{}' -o '{}' -w 1200 -H 800 2>&1 || true", mmdfile, output_path.string());
                ShellResult r2 = run_shell(cmd, root, 60);
                if (r2.exit_code == 0 && fs::exists(output_path)) {
                    auto sz = fs::file_size(output_path);
                    return std::format("[ok: generated SVG '{}' ({} bytes)]\n{}", output, sz, r2.output);
                }
                return std::format("[error: mmdc failed]\n{}", r2.output);
            }

            // Fallback: SVG with mermaid source as text
            std::string svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 800 600\" width=\"800\" height=\"600\">\n"
                "  <rect width=\"100%\" height=\"100%\" fill=\"#f8f9fa\"/>\n"
                "  <text x=\"400\" y=\"50\" text-anchor=\"middle\" font-family=\"monospace\" font-size=\"20\" fill=\"#333\">Mermaid Diagram</text>\n"
                "  <text x=\"400\" y=\"80\" text-anchor=\"middle\" font-family=\"monospace\" font-size=\"14\" fill=\"#666\">Install mmdc for proper SVG rendering</text>\n";
            int y = 120;
            svg += std::format("  <text x=\"20\" y=\"{}\" font-family=\"monospace\" font-size=\"12\" fill=\"#333\">Mermaid source:</text>\n", y);
            y += 25;
            std::istringstream ms(mermaid);
            std::string line;
            while (std::getline(ms, line)) {
                std::string esc;
                for (char c : line) {
                    if (c == '&') esc += "&amp;";
                    else if (c == '<') esc += "&lt;";
                    else if (c == '>') esc += "&gt;";
                    else if (c == '"') esc += "&quot;";
                    else if (c == '\'') esc += "&apos;";
                    else esc.push_back(c);
                }
                svg += std::format("  <text x=\"30\" y=\"{}\" font-family=\"monospace\" font-size=\"11\" fill=\"#555\">{}</text>\n", y, esc);
                y += 18;
            }
            svg += "</svg>\n";

            { std::ofstream of(output_path); of << svg; }
            return std::format("[ok: generated text-based SVG '{}' ({} bytes). Install mmdc for proper rendering.]\n"
                               "To install: npm install -g @mermaid-js/mermaid-cli",
                               output, svg.size());
        }

        // ── image_info ───────────────────────────────────────────────
        if (name == "image_info") {
            std::string path = get_str("path");
            if (path.empty()) return "[tool error: 'path' required]";
            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());

            std::string py_script = "import sys, json\n"
                "from PIL import Image\n"
                "img = Image.open(sys.argv[1])\n"
                "info = {'format': img.format or 'unknown', 'mode': img.mode, 'width': img.width, 'height': img.height}\n"
                "print(json.dumps(info, indent=2))\n";

            std::string pyfile = (fs::temp_directory_path() / "agent_img_info.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string cmd = std::format("python3 '{}' '{}' 2>&1 || true", pyfile, resolved.string());
            ShellResult r = run_shell(cmd, root, 30);
            if (r.exit_code != 0 || r.output.empty()) {
                return std::format("[error: could not read image '{}']\n{}", path, r.output);
            }
            return r.output;
        }

        // ── image_convert ────────────────────────────────────────────
        if (name == "image_convert") {
            std::string src = get_str("source");
            std::string dst = get_str("destination");
            int width = get_int("width", 0);
            int height = get_int("height", 0);
            int quality = std::clamp(get_int("quality", 85), 1, 100);
            if (src.empty() || dst.empty()) return "[tool error: 'source' and 'destination' required]";

            fs::path src_resolved = resolve_under_root(root, src);
            fs::path dst_resolved = resolve_under_root(root, dst);
            std::error_code ec;
            if (!fs::exists(src_resolved, ec)) return std::format("[error: source not found: {}]", src);
            fs::create_directories(dst_resolved.parent_path(), ec);

            std::string py_script = "import sys\n"
                "from PIL import Image\n"
                "src = sys.argv[1]; dst = sys.argv[2]\n"
                "img = Image.open(src)\n"
                "w = int(sys.argv[3]) if len(sys.argv)>3 and sys.argv[3] else 0\n"
                "h = int(sys.argv[4]) if len(sys.argv)>4 and sys.argv[4] else 0\n"
                "q = int(sys.argv[5]) if len(sys.argv)>5 and sys.argv[5] else 85\n"
                "if w or h:\n"
                "  if w and h: img = img.resize((w,h), Image.LANCZOS)\n"
                "  elif w: r = w/img.width; img = img.resize((w, int(img.height*r)), Image.LANCZOS)\n"
                "  elif h: r = h/img.height; img = img.resize((int(img.width*r), h), Image.LANCZOS)\n"
                "ext = dst.lower()\n"
                "kw = {}\n"
                "if ext.endswith('.jpg') or ext.endswith('.jpeg'):\n"
                "  kw['quality'] = q\n"
                "  if img.mode in ('RGBA','P'): img = img.convert('RGB')\n"
                "elif ext.endswith('.webp'): kw['quality'] = q\n"
                "elif ext.endswith('.png'): kw['compress_level'] = 6\n"
                "img.save(dst, **kw)\n"
                "print(f'OK: {img.width}x{img.height} -> {dst}')\n";

            std::string pyfile = (fs::temp_directory_path() / "agent_img_convert.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string cmd = std::format("python3 '{}' '{}' '{}' {} {} {} 2>&1 || true",
                                          pyfile, src_resolved.string(), dst_resolved.string(),
                                          width, height, quality);
            ShellResult r = run_shell(cmd, root, 60);
            if (r.exit_code != 0 || r.output.find("OK:") == std::string::npos) {
                return std::format("[error: conversion failed]\n{}", r.output);
            }
            auto out_size = fs::file_size(dst_resolved, ec);
            return std::format("[ok: converted '{}' -> '{}' ({} bytes)]\n{}", src, dst, out_size, r.output);
        }

        // ── image_to_svg ─────────────────────────────────────────────
        if (name == "image_to_svg") {
            std::string src = get_str("source");
            std::string dst = get_str("destination");
            if (src.empty() || dst.empty()) return "[tool error: 'source' and 'destination' required]";

            fs::path src_resolved = resolve_under_root(root, src);
            fs::path dst_resolved = resolve_under_root(root, dst);
            std::error_code ec;
            if (!fs::exists(src_resolved, ec)) return std::format("[error: source not found: {}]", src);
            fs::create_directories(dst_resolved.parent_path(), ec);

            std::string py_script = "import sys, base64, io\n"
                "from PIL import Image\n"
                "src = sys.argv[1]; dst = sys.argv[2]\n"
                "img = Image.open(src)\n"
                "fmt = img.format or 'PNG'\n"
                "mm = {'PNG':'image/png','JPEG':'image/jpeg','GIF':'image/gif','BMP':'image/bmp','WEBP':'image/webp'}\n"
                "mime = mm.get(fmt, 'image/png')\n"
                "buf = io.BytesIO()\n"
                "img.save(buf, format=fmt)\n"
                "b64 = base64.b64encode(buf.getvalue()).decode('ascii')\n"
                "svg = '<?xml version=\"1.0\" encoding=\"UTF-8\"?>\\n' \\\n"
                "  '<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 {} {}\" width=\"{}\" height=\"{}\">\\n' \\\n"
                "  '<image width=\"{}\" height=\"{}\" href=\"data:{};base64,{}\"/>\\n</svg>\\n' \\\n"
                "  .format(img.width, img.height, img.width, img.height, img.width, img.height, mime, b64)\n"
                "with open(dst, 'w') as f: f.write(svg)\n"
                "print(f'OK: {img.width}x{img.height} {fmt} -> SVG ({len(svg)} bytes)')\n";

            std::string pyfile = (fs::temp_directory_path() / "agent_img2svg.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string cmd = std::format("python3 '{}' '{}' '{}' 2>&1 || true",
                                          pyfile, src_resolved.string(), dst_resolved.string());
            ShellResult r = run_shell(cmd, root, 60);
            if (r.exit_code != 0 || r.output.find("OK:") == std::string::npos) {
                return std::format("[error: conversion failed]\n{}", r.output);
            }
            auto out_size = fs::file_size(dst_resolved, ec);
            return std::format("[ok: '{}' -> '{}' ({} bytes)]\n{}", src, dst, out_size, r.output);
        }

        // ── clipboard ────────────────────────────────────────────────
        if (name == "clipboard") {
            std::string action = get_str("action");
            if (action.empty()) return "[tool error: 'action' required (get or set)]";
            if (action == "get") {
                ShellResult r = run_shell("termux-clipboard-get 2>/dev/null || true", root, 10);
                if (r.output.empty()) return "[clipboard is empty]";
                return std::format("[clipboard content ({} bytes)]\n{}", r.output.size(), r.output);
            } else if (action == "set") {
                std::string content = get_str("content");
                if (content.empty()) return "[tool error: 'content' required when action='set']";
                std::string escaped;
                for (char c : content) {
                    if (c == '\'') escaped += "'\\''";
                    else escaped.push_back(c);
                }
                std::string cmd = std::format("printf '%s' '{}' | termux-clipboard-set 2>&1 || true", escaped);
                ShellResult r = run_shell(cmd, root, 10);
                if (r.exit_code == 0) return std::format("[ok: wrote {} bytes to clipboard]", content.size());
                return std::format("[error: failed to set clipboard]\n{}", r.output);
            }
            return std::format("[tool error: unknown action '{}' (use 'get' or 'set')]", action);
        }

        // ── notify ───────────────────────────────────────────────────
        if (name == "notify") {
            std::string title = get_str("title");
            std::string content = get_str("content");
            std::string priority = get_str("priority");
            bool alert_once = get_bool("alert_once", true);
            bool sound = get_bool("sound", true);
            if (title.empty()) return "[tool error: 'title' required]";
            if (content.empty()) return "[tool error: 'content' required]";
            if (priority.empty()) priority = "normal";

            auto esc = [](const std::string& s) {
                std::string r;
                for (char c : s) {
                    if (c == '\'') r += "'\\''";
                    else r.push_back(c);
                }
                return r;
            };

            std::string cmd = std::format("termux-notification --title '{}' --content '{}' --priority {} {} {} 2>&1 || true",
                                          esc(title), esc(content), esc(priority),
                                          alert_once ? "--alert-once" : "",
                                          sound ? "--sound" : "");
            ShellResult r = run_shell(cmd, root, 10);
            if (r.exit_code == 0) return std::format("[ok: notification sent: '{}']", title);
            return std::format("[error: notification failed]\n{}", r.output);
        }

        // ── vibrate ──────────────────────────────────────────────────
        if (name == "vibrate") {
            int dur = std::clamp(get_int("duration_ms", 200), 50, 10000);
            std::string cmd = std::format("termux-vibrate -d {} 2>&1 || true", dur);
            ShellResult r = run_shell(cmd, root, dur / 1000 + 5);
            if (r.exit_code == 0) return std::format("[ok: vibrated for {}ms]", dur);
            return std::format("[error: vibration failed]\n{}", r.output);
        }

        // ── run_python ───────────────────────────────────────────────
        if (name == "run_python") {
            std::string code = get_str("code");
            int timeout = std::clamp(get_int("timeout", 30), 5, 120);
            if (code.empty()) return "[tool error: 'code' required]";

            std::string pyfile = (fs::temp_directory_path() / "agent_run_python.py").string();
            {
                std::ofstream pf(pyfile);
                pf << "import sys, json, math, random, datetime, os, re, collections, itertools, statistics\n";
                pf << code << "\n";
            }

            std::string cmd = std::format("python3 '{}' 2>&1 || true", pyfile);
            ShellResult r = run_shell(cmd, root, timeout);
            if (r.output.empty()) r.output = "(no output)\n";
            return std::format("[python exit_code={}]\n{}", r.exit_code, truncate(r.output, 100000));
        }

        // ── ocr ──────────────────────────────────────────────────────
        if (name == "ocr") {
            std::string image_path = get_str("image_path");
            std::string lang = get_str("lang");
            if (image_path.empty()) return "[tool error: 'image_path' required]";
            if (lang.empty()) lang = "eng";

            fs::path resolved = resolve_under_root(root, image_path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());

            std::string escaped_path;
            for (char c : resolved.string()) {
                if (c == '\'') escaped_path += "'\\''";
                else escaped_path.push_back(c);
            }

            std::string cmd = std::format("tesseract '{}' stdout -l '{}' 2>/dev/null || true", escaped_path, lang);
            ShellResult r = run_shell(cmd, root, 60);
            if (r.exit_code != 0 || r.output.empty()) {
                return std::format("[error: OCR failed. Is tesseract installed? Try: pkg install tesseract]\n{}", r.output);
            }
            return std::format("[OCR result from '{}' (lang={})]\n{}", image_path, lang, r.output);
        }

        // ── qr_encode ────────────────────────────────────────────────
        if (name == "qr_encode") {
            std::string data = get_str("data");
            std::string output = get_str("output");
            if (data.empty()) return "[tool error: 'data' required]";
            if (output.empty()) return "[tool error: 'output' required]";

            fs::path out_path = resolve_under_root(root, output);
            fs::create_directories(out_path.parent_path());

            std::string py_script = "import sys, qrcode\n"
                "data = sys.argv[1]\n"
                "out = sys.argv[2]\n"
                "img = qrcode.make(data)\n"
                "img.save(out)\n"
                "print(f'OK: QR code saved to {out}')\n";

            std::string pyfile = (fs::temp_directory_path() / "agent_qr_encode.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string escaped_data;
            for (char c : data) {
                if (c == '\'') escaped_data += "'\\''";
                else escaped_data.push_back(c);
            }

            std::string cmd = std::format("python3 '{}' '{}' '{}' 2>&1 || true",
                                          pyfile, escaped_data, out_path.string());
            ShellResult r = run_shell(cmd, root, 30);
            if (r.exit_code != 0 || r.output.find("OK:") == std::string::npos) {
                return std::format("[error: QR code generation failed]\n{}", r.output);
            }
            std::error_code ec;
            auto sz = fs::file_size(out_path, ec);
            return std::format("[ok: QR code generated -> '{}' ({} bytes)]\nData: {}", output, sz, data);
        }

        // ── qr_decode ────────────────────────────────────────────────
        if (name == "qr_decode") {
            std::string image_path = get_str("image_path");
            if (image_path.empty()) return "[tool error: 'image_path' required]";

            fs::path resolved = resolve_under_root(root, image_path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());

            std::string py_script = "import sys\n"
                "try:\n"
                "  from PIL import Image\n"
                "  from pyzbar.pyzbar import decode\n"
                "  img = Image.open(sys.argv[1])\n"
                "  results = decode(img)\n"
                "  if results:\n"
                "    for r in results:\n"
                "      print(f'Type: {r.type}, Data: {r.data.decode(\"utf-8\", errors=\"replace\")}')\n"
                "  else:\n"
                "    print('NO_RESULT')\n"
                "except ImportError:\n"
                "  print('NO_PYZBAR')\n";

            std::string pyfile = (fs::temp_directory_path() / "agent_qr_decode.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string cmd = std::format("python3 '{}' '{}' 2>&1 || true", pyfile, resolved.string());
            ShellResult r = run_shell(cmd, root, 30);

            if (r.output.find("NO_PYZBAR") != std::string::npos) {
                std::string escaped_path;
                for (char c : resolved.string()) {
                    if (c == '\'') escaped_path += "'\\''";
                    else escaped_path.push_back(c);
                }
                cmd = std::format("zbarimg --quiet '{}' 2>/dev/null || true", escaped_path);
                r = run_shell(cmd, root, 30);
                if (r.output.empty()) return "[no barcode/QR code found in image]";
                return std::format("[barcode/QR result from '{}']\n{}", image_path, r.output);
            }

            if (r.output.find("NO_RESULT") != std::string::npos) {
                return std::format("[no barcode/QR code found in '{}']", image_path);
            }
            if (r.output.empty()) return std::format("[error: could not decode '{}']", image_path);
            return std::format("[barcode/QR result from '{}']\n{}", image_path, r.output);
        }

        // ── diff_files ───────────────────────────────────────────────
        if (name == "diff_files") {
            std::string f1 = get_str("file1");
            std::string f2 = get_str("file2");
            int ctx = std::max(0, get_int("context_lines", 3));
            if (f1.empty() || f2.empty()) return "[tool error: 'file1' and 'file2' required]";

            fs::path f1_res = resolve_under_root(root, f1);
            fs::path f2_res = resolve_under_root(root, f2);
            std::error_code ec;
            if (!fs::exists(f1_res, ec)) return std::format("[error: file not found: {}]", f1);
            if (!fs::exists(f2_res, ec)) return std::format("[error: file not found: {}]", f2);

            auto esc = [](const fs::path& p) {
                std::string s = p.string();
                std::string r;
                for (char c : s) {
                    if (c == '\'') r += "'\\''";
                    else r.push_back(c);
                }
                return r;
            };

            std::string cmd = std::format("diff -u -U{} '{}' '{}' 2>/dev/null || true", ctx, esc(f1_res), esc(f2_res));
            ShellResult r = run_shell(cmd, root, 30);
            if (r.output.empty()) return "[files are identical]";
            return truncate(r.output, 100000);
        }

        // ── compress ─────────────────────────────────────────────────
        if (name == "compress") {
            std::string src = get_str("source");
            std::string output = get_str("output");
            std::string format = get_str("format");
            if (src.empty()) return "[tool error: 'source' required]";
            if (output.empty()) return "[tool error: 'output' required]";

            fs::path src_res = resolve_under_root(root, src);
            fs::path out_res = resolve_under_root(root, output);
            std::error_code ec;
            if (!fs::exists(src_res, ec)) return std::format("[error: source not found: {}]", src);
            fs::create_directories(out_res.parent_path(), ec);

            if (format.empty()) {
                std::string ext = out_res.extension().string();
                if (ext == ".zip") format = "zip";
                else if (ext == ".gz" || ext == ".tgz" || ext == ".tar.gz") format = "tar.gz";
                else format = "zip";
            }

            auto esc = [](const fs::path& p) {
                std::string s = p.string();
                std::string r;
                for (char c : s) {
                    if (c == '\'') r += "'\\''";
                    else r.push_back(c);
                }
                return r;
            };

            std::string cmd;
            if (format == "zip") {
                if (fs::is_directory(src_res, ec)) {
                    cmd = std::format("cd '{}' && zip -r '{}' . 2>&1 || true",
                                      esc(src_res), esc(out_res));
                } else {
                    cmd = std::format("zip '{}' '{}' 2>&1 || true", esc(out_res), esc(src_res));
                }
            } else if (format == "tar.gz") {
                std::string parent = src_res.parent_path().string();
                std::string name = src_res.filename().string();
                cmd = std::format("cd '{}' && tar -czf '{}' '{}' 2>&1 || true",
                                  esc(fs::path(parent)), esc(out_res), name);
            } else {
                return std::format("[error: unsupported format '{}'. Use 'zip' or 'tar.gz']", format);
            }

            ShellResult r = run_shell(cmd, root, 120);
            if (r.exit_code != 0 || !fs::exists(out_res, ec)) {
                return std::format("[error: compression failed]\n{}", r.output);
            }
            auto sz = fs::file_size(out_res, ec);
            return std::format("[ok: compressed '{}' -> '{}' ({} bytes)]\n{}", src, output, sz, r.output);
        }

        // ── decompress ───────────────────────────────────────────────
        if (name == "decompress") {
            std::string archive = get_str("archive");
            std::string output_dir = get_str("output_dir");
            if (archive.empty()) return "[tool error: 'archive' required]";

            fs::path arc_res = resolve_under_root(root, archive);
            std::error_code ec;
            if (!fs::exists(arc_res, ec)) return std::format("[error: archive not found: {}]", archive);

            if (output_dir.empty()) {
                output_dir = arc_res.stem().string();
                std::string fn = arc_res.filename().string();
                if (fn.size() > 7 && fn.substr(fn.size()-7) == ".tar.gz") {
                    output_dir = fn.substr(0, fn.size()-7);
                }
            }
            fs::path out_res = resolve_under_root(root, output_dir);
            fs::create_directories(out_res, ec);

            auto esc = [](const fs::path& p) {
                std::string s = p.string();
                std::string r;
                for (char c : s) {
                    if (c == '\'') r += "'\\''";
                    else r.push_back(c);
                }
                return r;
            };

            std::string ext = arc_res.extension().string();
            std::string fn = arc_res.filename().string();
            std::string cmd;
            if (ext == ".zip") {
                cmd = std::format("unzip -o '{}' -d '{}' 2>&1 || true", esc(arc_res), esc(out_res));
            } else if (ext == ".gz" || ext == ".tgz" || (fn.size() > 7 && fn.substr(fn.size()-7) == ".tar.gz")) {
                cmd = std::format("tar -xzf '{}' -C '{}' 2>&1 || true", esc(arc_res), esc(out_res));
            } else if (ext == ".bz2" || (fn.size() > 8 && fn.substr(fn.size()-8) == ".tar.bz2")) {
                cmd = std::format("tar -xjf '{}' -C '{}' 2>&1 || true", esc(arc_res), esc(out_res));
            } else if (ext == ".xz" || (fn.size() > 7 && fn.substr(fn.size()-7) == ".tar.xz")) {
                cmd = std::format("tar -xJf '{}' -C '{}' 2>&1 || true", esc(arc_res), esc(out_res));
            } else {
                cmd = std::format("tar -xf '{}' -C '{}' 2>&1 || true", esc(arc_res), esc(out_res));
            }

            ShellResult r = run_shell(cmd, root, 120);
            if (r.exit_code != 0) {
                return std::format("[error: extraction may have failed]\n{}", r.output);
            }
            return std::format("[ok: extracted '{}' -> '{}']\n{}", archive, output_dir, r.output);
        }

        // ── system_info ──────────────────────────────────────────────
        if (name == "system_info") {
            std::string category = get_str("category");
            if (category.empty()) category = "all";

            std::ostringstream ss;

            if (category == "all" || category == "battery") {
                ShellResult r = run_shell("termux-battery-status 2>/dev/null || true", root, 10);
                if (!r.output.empty()) {
                    ss << "=== Battery ===\n" << r.output << "\n";
                } else {
                    ss << "=== Battery ===\n(termux-battery-status not available)\n\n";
                }
            }

            if (category == "all" || category == "cpu") {
                ShellResult r = run_shell("cat /proc/cpuinfo 2>/dev/null | head -30 || true", root, 5);
                if (!r.output.empty()) ss << "=== CPU ===\n" << r.output << "\n";
                ShellResult r2 = run_shell("cat /proc/loadavg 2>/dev/null || true", root, 5);
                if (!r2.output.empty()) ss << "Load: " << r2.output << "\n";
            }

            if (category == "all" || category == "memory") {
                ShellResult r = run_shell("cat /proc/meminfo 2>/dev/null | head -10 || true", root, 5);
                if (!r.output.empty()) ss << "=== Memory ===\n" << r.output << "\n";
            }

            if (category == "all" || category == "storage") {
                ShellResult r = run_shell("df -h /data/data/com.termux/files 2>/dev/null || df -h / 2>/dev/null || true", root, 5);
                if (!r.output.empty()) ss << "=== Storage ===\n" << r.output << "\n";
            }

            if (category == "all" || category == "network") {
                ShellResult r = run_shell("ip addr show 2>/dev/null | head -30 || ifconfig 2>/dev/null || true", root, 5);
                if (!r.output.empty()) ss << "=== Network ===\n" << r.output << "\n";
            }

            if (category == "all") {
                ShellResult r = run_shell("uname -a 2>/dev/null || true", root, 5);
                if (!r.output.empty()) ss << "=== System ===\n" << r.output << "\n";
                ShellResult r2 = run_shell("getprop ro.build.version.release 2>/dev/null || echo 'unknown'", root, 5);
                if (!r2.output.empty()) ss << "Android: " << r2.output;
            }

            return ss.str();
        }

        // ── weather ──────────────────────────────────────────────────
        if (name == "weather") {
            std::string location = get_str("location");

            std::string url;
            if (location.empty()) {
                url = "https://wttr.in?format=4&m";
            } else {
                std::string escaped;
                for (char c : location) {
                    if (c == '\'') escaped += "'\\''";
                    else escaped.push_back(c);
                }
                url = std::format("https://wttr.in/{}?format=4&m", escaped);
            }

            std::string escaped_url;
            for (char c : url) {
                if (c == '\'') escaped_url += "'\\''";
                else escaped_url.push_back(c);
            }

            std::string cmd = std::format("curl -sL --max-time 15 '{}' 2>/dev/null || true", escaped_url);
            ShellResult r = run_shell(cmd, root, 20);
            std::string short_weather = r.output;

            std::string detail_url;
            if (location.empty()) {
                detail_url = "https://wttr.in?m";
            } else {
                detail_url = std::format("https://wttr.in/{}?m", escaped_url);
            }
            std::string escaped_detail;
            for (char c : detail_url) {
                if (c == '\'') escaped_detail += "'\\''";
                else escaped_detail.push_back(c);
            }
            cmd = std::format("curl -sL --max-time 15 '{}' 2>/dev/null | head -50 || true", escaped_detail);
            ShellResult r2 = run_shell(cmd, root, 20);

            std::ostringstream ss;
            if (!short_weather.empty()) ss << short_weather << "\n";
            if (!r2.output.empty()) ss << r2.output;
            if (ss.str().empty()) return "[error: could not fetch weather. Try specifying a city name.]";
            return truncate(ss.str(), 5000);
        }

        // ── get_location - get current geographic location ────────────
        if (name == "get_location") {
            std::string provider = get_str("provider");
            if (provider.empty()) provider = "network";

            if (provider != "network" && provider != "gps") {
                return std::format("[tool error: invalid provider '{}'. Use 'network' or 'gps']", provider);
            }

            std::string cmd = std::format("termux-location -p {} 2>&1 || true", provider);
            ShellResult r = run_shell(cmd, root, provider == "gps" ? 60 : 15);

            if (r.exit_code != 0 || r.output.empty()) {
                return std::format("[error: could not get location. Is termux-api installed? Try: pkg install termux-api]\n{}", r.output);
            }

            // Parse and pretty-print the JSON result
            std::string py_script = R"PY(
import sys, json
try:
    data = json.load(sys.stdin)
    lat = data.get('latitude', '?')
    lon = data.get('longitude', '?')
    acc = data.get('accuracy', '?')
    prov = data.get('provider', '?')
    alt = data.get('altitude', '?')
    print(f"Latitude:  {lat}")
    print(f"Longitude: {lon}")
    print(f"Accuracy:  {acc}m")
    print(f"Provider:  {prov}")
    if alt: print(f"Altitude:  {alt}m")
except Exception as e:
    print(f"[parse error: {e}]")
    print(sys.stdin.read())
)PY";

            std::string pyfile = (fs::temp_directory_path() / "agent_location.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string jsonfile = (fs::temp_directory_path() / "agent_location_in.json").string();
            { std::ofstream jf(jsonfile); jf << r.output; }
            cmd = std::format("python3 '{}' < '{}' 2>&1 || true", pyfile, jsonfile);
            ShellResult r2 = run_shell(cmd, root, 10);

            if (!r2.output.empty()) return r2.output;
            return r.output;
        }

        // ── get_datetime - get current date and time ──────────────────
        if (name == "get_datetime") {
            std::string format = get_str("format");
            if (format.empty()) format = "%Y-%m-%d %H:%M:%S %Z";

            // Escape single quotes for shell
            std::string escaped_fmt;
            for (char c : format) {
                if (c == '\'') escaped_fmt += "'\\''";
                else escaped_fmt.push_back(c);
            }

            std::string cmd = std::format("date '+{}' 2>&1 || true", escaped_fmt);
            ShellResult r = run_shell(cmd, root, 5);

            if (r.exit_code != 0 || r.output.empty()) {
                return std::format("[error: could not get date/time]\n{}", r.output);
            }

            // Also get timezone and timestamp info
            std::string cmd_tz = "date '+%Z' 2>/dev/null || true";
            ShellResult r_tz = run_shell(cmd_tz, root, 5);

            std::string cmd_epoch = "date '+%s' 2>/dev/null || true";
            ShellResult r_epoch = run_shell(cmd_epoch, root, 5);

            std::ostringstream ss;
            ss << r.output;
            if (!ss.str().empty() && ss.str().back() != '\n') ss << '\n';
            if (!r_tz.output.empty()) {
                std::string tz = r_tz.output;
                while (!tz.empty() && (tz.back() == '\n' || tz.back() == '\r')) tz.pop_back();
                ss << std::format("Timezone: {}\n", tz);
            }
            if (!r_epoch.output.empty()) {
                std::string ep = r_epoch.output;
                while (!ep.empty() && (ep.back() == '\n' || ep.back() == '\r')) ep.pop_back();
                ss << std::format("Unix timestamp: {}\n", ep);
            }
            return ss.str();
        }
        // ── screenshot ───────────────────────────────────────────────
        if (name == "screenshot") {
            std::string output = get_str("output");
            if (output.empty()) output = "screenshot.png";

            fs::path out_path = resolve_under_root(root, output);
            fs::create_directories(out_path.parent_path());

            std::string cmd = std::format("termux-screencap '{}' 2>&1 || true", out_path.string());
            ShellResult r = run_shell(cmd, root, 30);
            std::error_code ec;
            if (fs::exists(out_path, ec) && fs::file_size(out_path, ec) > 0) {
                auto sz = fs::file_size(out_path, ec);
                return std::format("[ok: screenshot saved to '{}' ({} bytes)]", output, sz);
            }
            return std::format("[error: screenshot failed]\n{}", r.output);
        }

        // ── plot_chart ───────────────────────────────────────────────
        if (name == "plot_chart") {
            std::string chart_type = get_str("chart_type");
            std::string data_json = get_str("data_json");
            std::string title = get_str("title");
            std::string output = get_str("output");
            if (chart_type.empty()) return "[tool error: 'chart_type' required (bar, line, pie, scatter)]";
            if (data_json.empty()) return "[tool error: 'data_json' required]";
            if (output.empty()) output = "chart.png";

            fs::path out_path = resolve_under_root(root, output);
            fs::create_directories(out_path.parent_path());

            if (chart_type != "bar" && chart_type != "line" && chart_type != "pie" && chart_type != "scatter") {
                return std::format("[error: invalid chart_type '{}'. Use: bar, line, pie, scatter]", chart_type);
            }

            std::string py_script = R"PY(
import sys, json, os
os.environ['MPLBACKEND'] = 'Agg'
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

chart_type = sys.argv[1]
data = json.loads(sys.argv[2])
title = sys.argv[3] if len(sys.argv) > 3 else ''
output = sys.argv[4] if len(sys.argv) > 4 else 'chart.png'

fig, ax = plt.subplots(figsize=(10, 6))

if chart_type == 'bar':
    labels = data.get('labels', [])
    values = data.get('values', [])
    colors = data.get('colors', None)
    ax.bar(labels, values, color=colors)
    ax.set_xlabel(data.get('xlabel', ''))
    ax.set_ylabel(data.get('ylabel', ''))

elif chart_type == 'line':
    labels = data.get('labels', [])
    values = data.get('values', [])
    ax.plot(labels, values, marker='o', linewidth=2)
    ax.set_xlabel(data.get('xlabel', ''))
    ax.set_ylabel(data.get('ylabel', ''))
    ax.grid(True, alpha=0.3)

elif chart_type == 'pie':
    labels = data.get('labels', [])
    values = data.get('values', [])
    ax.pie(values, labels=labels, autopct='%1.1f%%', startangle=90)
    ax.axis('equal')

elif chart_type == 'scatter':
    x = data.get('x', [])
    y = data.get('y', [])
    ax.scatter(x, y, alpha=0.6)
    ax.set_xlabel(data.get('xlabel', ''))
    ax.set_ylabel(data.get('ylabel', ''))
    ax.grid(True, alpha=0.3)

if title:
    ax.set_title(title)

plt.tight_layout()
plt.savefig(output, dpi=150)
print(f'OK: chart saved to {output}')
)PY";

            std::string pyfile = (fs::temp_directory_path() / "agent_plot.py").string();
            { std::ofstream pf(pyfile); pf << py_script; }

            std::string escaped_data;
            for (char c : data_json) {
                if (c == '\'') escaped_data += "'\\''";
                else escaped_data.push_back(c);
            }
            std::string escaped_title;
            for (char c : title) {
                if (c == '\'') escaped_title += "'\\''";
                else escaped_title.push_back(c);
            }

            std::string cmd = std::format("python3 '{}' '{}' '{}' '{}' '{}' 2>&1 || true",
                                          pyfile, chart_type, escaped_data, escaped_title, out_path.string());
            ShellResult r = run_shell(cmd, root, 60);
            std::error_code ec;
            if (fs::exists(out_path, ec) && fs::file_size(out_path, ec) > 0) {
                auto sz = fs::file_size(out_path, ec);
                return std::format("[ok: {} chart generated -> '{}' ({} bytes)]\n{}", chart_type, output, sz, r.output);
            }
            return std::format("[error: chart generation failed]\n{}", r.output);
        }

        // ── show_image - display an image file in Termux ──────────────
        if (name == "show_image") {
            std::string path = get_str("path");
            std::string method = get_str("method");
            int ascii_width = std::clamp(get_int("ascii_width", 0), 0, 200);
            if (path.empty()) return "[tool error: 'path' required]";
            if (method.empty()) method = "auto";

            fs::path resolved = resolve_under_root(root, path);
            std::error_code ec;
            if (!fs::exists(resolved, ec)) return std::format("[error: file not found: {}]", resolved.string());

            // Check if it's an SVG file - convert to PNG first
            fs::path display_path = resolved;
            std::string ext = resolved.extension().string();
            bool is_svg = (ext == ".svg" || ext == ".SVG");

            if (is_svg) {
                // Convert SVG to PNG using Python's cairosvg or rsvg-convert
                std::string png_path = (fs::temp_directory_path() / "agent_svg_temp.png").string();
                std::string escaped_src, escaped_dst;
                for (char c : resolved.string()) { if (c == '\'') escaped_src += "'\\''"; else escaped_src.push_back(c); }
                for (char c : png_path) { if (c == '\'') escaped_dst += "'\\''"; else escaped_dst.push_back(c); }

                // Try rsvg-convert first, then cairosvg, then fallback
                // Try rsvg-convert first, then ImageMagick convert, as fallback
                std::string svg_cmd = std::format("rsvg-convert '{}' -o '{}' 2>/dev/null || convert '{}' '{}' 2>/dev/null || true",
                                                  escaped_src, escaped_dst, escaped_src, escaped_dst);
                ShellResult svg_r = run_shell(svg_cmd, root, 30);
                if (fs::exists(png_path, ec) && fs::file_size(png_path, ec) > 0) {
                    display_path = png_path;
                } else {
                    return std::format("[error: cannot convert SVG '{}' to PNG. Install rsvg-convert: pkg install librsvg]", path);
                }
            }

            // Determine method
            if (method == "auto") {
                // Check if termux-open is available
                std::string check_cmd = "command -v termux-open 2>/dev/null || true";
                ShellResult check_r = run_shell(check_cmd, root, 5);
                if (!check_r.output.empty() && check_r.output.find("termux-open") != std::string::npos) {
                    method = "termux-open";
                } else {
                    method = "ascii";
                }
            }

            if (method == "termux-open") {
                std::string escaped;
                for (char c : display_path.string()) { if (c == '\'') escaped += "'\\''"; else escaped.push_back(c); }
                std::string cmd = std::format("termux-open '{}' 2>&1 || true", escaped);
                ShellResult r = run_shell(cmd, root, 10);
                if (r.exit_code == 0) {
                    std::ostringstream ss;
                    ss << std::format("[ok: opened '{}' with system image viewer]", path);
                    if (is_svg) ss << std::format(" (converted from SVG)");
                    return ss.str();
                }
                // Fallback to ascii if termux-open fails
                method = "ascii";
            }

            if (method == "ascii") {
                // Use Python PIL to render image as ASCII art
                if (ascii_width == 0) {
                    // Try to get terminal width
                    ShellResult tw_r = run_shell("tput cols 2>/dev/null || echo 80", root, 5);
                    try { ascii_width = std::stoi(tw_r.output); } catch (...) { ascii_width = 80; }
                    ascii_width = std::clamp(ascii_width, 40, 200);
                }

                std::string py_script = R"PY(
import sys, os
os.environ['MPLBACKEND'] = 'Agg'
from PIL import Image

img_path = sys.argv[1]
width = int(sys.argv[2]) if len(sys.argv) > 2 else 80

# ASCII characters from dark to light
chars = ' .:-=+*#%@'
# chars = ' .\'`^",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$'

try:
    img = Image.open(img_path)
    
    # Convert to grayscale
    if img.mode == 'RGBA':
        # Handle transparency: composite on white background
        bg = Image.new('RGB', img.size, (255, 255, 255))
        bg.paste(img, mask=img.split()[3])
        img = bg
    img = img.convert('L')
    
    # Calculate height to preserve aspect ratio (characters are ~2x tall as wide)
    aspect = img.height / img.width
    height = max(1, int(width * aspect * 0.45))
    
    img = img.resize((width, height), Image.LANCZOS)
    pixels = list(img.getdata())
    
    result = []
    for y in range(height):
        row = ''
        for x in range(width):
            pixel = pixels[y * width + x]
            # Map pixel value (0-255) to character index
            idx = pixel * (len(chars) - 1) // 255
            row += chars[idx]
        result.append(row)
    
    print(f'ASCII art ({width}x{height}):')
    print('\n'.join(result))
    print(f'\n[Image: {img_path}, size={width}x{height}]')
except Exception as e:
    print(f'[error: {e}]', file=sys.stderr)
    sys.exit(1)
)PY";

                std::string pyfile = (fs::temp_directory_path() / "agent_show_image.py").string();
                { std::ofstream pf(pyfile); pf << py_script; }

                std::string escaped_path;
                for (char c : display_path.string()) { if (c == '\'') escaped_path += "'\\''"; else escaped_path.push_back(c); }

                std::string cmd = std::format("python3 '{}' '{}' {} 2>&1 || true", pyfile, escaped_path, ascii_width);
                ShellResult r = run_shell(cmd, root, 30);

                if (r.exit_code == 0 && !r.output.empty()) {
                    std::ostringstream ss;
                    ss << r.output;
                    if (is_svg) ss << "\n[converted from SVG]";
                    return ss.str();
                }
                return std::format("[error: ASCII render failed]\n{}", r.output);
            }

            return std::format("[tool error: unknown method '{}'. Use 'auto', 'termux-open', or 'ascii']", method);
        }

        return std::format("[tool error: unknown tool '{}']", name);
    } catch (const std::exception& e) {
        return std::format("[tool error: {}]", e.what());
    }
}

} // namespace tools
