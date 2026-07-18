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

    // 1. read_file - supports large files with offset/limit
    {
        json::Object p;
        p["type"] = json::Value{"object"};
        json::Object props;
        json::Object path_p; path_p["type"] = json::Value{"string"};
        path_p["description"] = json::Value{"Path relative to workspace root (or absolute)."};
        props["path"] = json::Value{std::move(path_p)};
        json::Object offset_p; offset_p["type"] = json::Value{"integer"};
        offset_p["description"] = json::Value{"Byte offset to start reading from (0 = beginning). Useful for large files."};
        offset_p["default"] = json::Value{0};
        props["offset"] = json::Value{std::move(offset_p)};
        json::Object limit_p; limit_p["type"] = json::Value{"integer"};
        limit_p["description"] = json::Value{"Maximum number of bytes to read. Omit or set to 0 to read until end (capped at max_bytes)."};
        limit_p["default"] = json::Value{0};
        props["limit"] = json::Value{std::move(limit_p)};
        json::Object maxb_p; maxb_p["type"] = json::Value{"integer"};
        maxb_p["description"] = json::Value{"Maximum total bytes to return (default 200000). Increase for very large files."};
        maxb_p["default"] = json::Value{200000};
        props["max_bytes"] = json::Value{std::move(maxb_p)};
        p["properties"] = json::Value{std::move(props)};
        p["required"] = json::make_array<std::string>({"path"});
        tools.push_back(fn("read_file",
            "Read file contents with optional offset/limit for large files. "
            "For small files just pass 'path'. For large files use offset and limit "
            "to read in chunks, or increase max_bytes.",
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

        return std::format("[tool error: unknown tool '{}']", name);
    } catch (const std::exception& e) {
        return std::format("[tool error: {}]", e.what());
    }
}

} // namespace tools
