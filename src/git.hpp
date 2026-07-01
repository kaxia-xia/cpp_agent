// git.hpp - Git integration for workspace versioning.
//
// Provides:
//   - Startup check: is git available? Is the workspace a git repo?
//   - Auto-commit: after each agent turn that modifies files, commit them
//     so the user can always `git checkout` or `git restore` to undo changes.
//   - Only files actually modified by the agent during the turn are committed;
//     pre-existing dirty files (modified before the turn) are left untouched.
//
#pragma once

#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace git {

namespace fs = std::filesystem;

// ── helpers ──────────────────────────────────────────────────────────

// Escape a string for safe use inside a Bourne-shell single-quoted string.
// The strategy: end the single-quote, emit the literal char escaped with
// backslash, then resume single-quoting.  This is the only portable way to
// embed any character inside a single-quoted shell string.
inline std::string shell_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'') {
            // "'" → '\''  (end quote, escaped quote, resume quote)
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Run a git command in the given directory, capturing output.
// Returns (exit_code, stdout+stderr).
inline std::pair<int, std::string> run_git(const fs::path& cwd,
                                            std::string_view args) {
    // Quote the directory path so spaces and special chars are safe.
    std::string cmd = std::format("cd '{}' && git {} 2>&1",
                                  shell_escape(cwd.string()), args);
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return {-1, "popen failed"};
    std::string out;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), fp)) out += buf;
    int rc = ::pclose(fp);
    return {rc, out};
}

// Split a string into lines.
inline std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : s) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Trim whitespace from both ends.
inline std::string trim(std::string_view s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

// ── public API ───────────────────────────────────────────────────────

// Check whether `git` is available on PATH. Returns true if found.
// Result is cached after the first call.
inline bool is_available() {
    static int cached = -1;  // -1 = unknown, 0 = no, 1 = yes
    if (cached == -1) {
        auto [rc, out] = run_git(fs::current_path(), "--version");
        cached = (rc == 0) ? 1 : 0;
    }
    return cached == 1;
}

// Ensure the workspace at `root` is a git repository.
// If no .git directory exists, initialise a new empty repo and print a hint.
// Returns true on success (repo exists or was created).
inline bool ensure_repo(const fs::path& root) {
    fs::path git_dir = root / ".git";
    if (fs::exists(git_dir)) {
        return true;  // already a repo
    }

    // No repo yet – create one.
    auto [rc, out] = run_git(root, "init");
    if (rc != 0) {
        std::cerr << std::format(
            "[git] warning: failed to initialise git repository: {}\n", out);
        return false;
    }

    // Create an initial commit so the working tree is clean.
    // First set a minimal user config if none is set.
    run_git(root, "config user.name  coding-agent");
    run_git(root, "config user.email coding-agent@local");

    // Add everything that exists and make the initial commit.
    run_git(root, "add -A");
    auto [rc4, out4] = run_git(root, "commit -m 'initial workspace' --allow-empty");

    std::cout << std::format(
        "[git] initialised empty git repository at {}\n"
        "[git] use `git status`, `git diff`, `git checkout` etc. to track and undo changes.\n",
        root.string());

    return true;
}

// Get the set of tracked files that currently have unstaged modifications
// (including untracked files that are not ignored).
// Returns a set of relative paths.
inline std::set<std::string> get_dirty_files(const fs::path& root) {
    std::set<std::string> dirty;

    // Tracked files with unstaged modifications.
    auto [rc1, out1] = run_git(root, "diff --name-only");
    if (rc1 == 0 && !out1.empty()) {
        for (auto& line : split_lines(out1)) {
            std::string f = trim(line);
            if (!f.empty()) dirty.insert(f);
        }
    }

    // Untracked files (not ignored).
    auto [rc2, out2] = run_git(root, "ls-files --others --exclude-standard");
    if (rc2 == 0 && !out2.empty()) {
        for (auto& line : split_lines(out2)) {
            std::string f = trim(line);
            if (!f.empty()) dirty.insert(f);
        }
    }

    return dirty;
}

// Get the full SHA hash of the current HEAD commit.
// Returns empty string on failure.
inline std::string get_head_hash(const fs::path& root) {
    auto [rc, out] = run_git(root, "rev-parse HEAD");
    if (rc != 0) return {};
    return trim(out);
}

// Reset the working tree to a specific commit hash.
// Uses `git reset --hard` to restore both the index and working tree.
// Returns true on success.
inline bool reset_to_commit(const fs::path& root, std::string_view hash) {
    if (hash.empty()) return false;
    auto [rc, out] = run_git(root, std::format("reset --hard {}", hash));
    if (rc != 0) {
        std::cerr << std::format("[git] warning: reset --hard failed: {}\n", out);
        return false;
    }
    // Also clean untracked files that were added by the agent.
    run_git(root, "clean -fd");
    return true;
}

// Commit only the files that were modified by the agent during this turn.
// `label` is a short human-readable description of the turn (e.g. the user prompt).
// `dirty_before` is the set of files that were already dirty before the turn started;
// these files will NOT be included in the commit.
// Returns the commit hash on success, empty string on failure.
inline std::string commit_changes(const fs::path& root, std::string_view label,
                                  const std::set<std::string>& dirty_before) {
    // Get the current set of dirty files.
    std::set<std::string> dirty_now = get_dirty_files(root);

    // Compute the set of files that are newly dirty (i.e. changed by the agent).
    std::set<std::string> agent_changed;
    for (const auto& f : dirty_now) {
        if (dirty_before.find(f) == dirty_before.end()) {
            agent_changed.insert(f);
        }
    }

    if (agent_changed.empty()) {
        // No new changes – nothing to commit.
        return {};
    }

    // Stage only the files the agent changed.
    // Build a single `git add` command with all the files.
    std::string add_args;
    for (const auto& f : agent_changed) {
        if (!add_args.empty()) add_args += ' ';
        add_args += shell_escape(f);
    }

    auto [rc_add, out_add] = run_git(root, std::format("add -- {}", add_args));
    if (rc_add != 0) {
        std::cerr << std::format("[git] warning: git add failed: {}\n", out_add);
        return {};
    }

    // Check if there is anything to commit (should always be true here, but be safe).
    auto [rc_diff, out_diff] = run_git(root, "diff --cached --stat");
    if (rc_diff != 0 || out_diff.empty()) {
        return {};
    }

    // Build a descriptive commit message.
    std::string msg = std::format("agent: {}", label);
    // Truncate to a reasonable length.
    if (msg.size() > 80) msg = msg.substr(0, 80) + "...";

    // Use shell-escaped message with -m to handle special characters safely.
    auto [rc_cm, out_cm] = run_git(root,
        std::format("commit -m '{}'", shell_escape(msg)));
    if (rc_cm != 0) {
        std::cerr << std::format("[git] warning: commit failed: {}\n", out_cm);
        return {};
    }

    // Get the commit hash of the new commit.
    std::string hash = get_head_hash(root);

    // Print a short summary of what was committed.
    std::cout << std::format("[git] committed: {}  ({})\n", msg, hash.empty() ? "?" : hash.substr(0, 12));
    // Show the files that were changed (diff --stat output is multi-line).
    auto lines = split_lines(out_diff);
    for (const auto& l : lines) {
        if (!l.empty()) std::cout << "  " << l << '\n';
    }

    return hash;
}

} // namespace git
