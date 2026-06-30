// markdown.hpp - lightweight Markdown → terminal ANSI renderer.
//
// Renders common Markdown constructs to colored terminal output:
//   - Headings (# to ######)  → bold + underline + color
//   - Tables (| ... |)        → aligned columns with borders
//   - Code blocks (```...```) → dim background / indented
//   - Inline code (`...`)     → dim background
//   - Bold (**...**)          → bold
//   - Italic (*...*)          → dim/italic
//   - Unordered lists (-/*)   → bullet indented
//   - Ordered lists (1.)      → numbered indented
//   - Horizontal rules (---)  → line
//   - Blockquotes (>)         → dim + prefix
//   - Links [text](url)       → text (underlined) + url dim
//
// All output goes to std::cout. No external dependencies.
#pragma once

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace markdown {

// Terminal color/style codes.
struct Style {
    std::string_view reset      = "\033[0m";
    std::string_view bold       = "\033[1m";
    std::string_view dim        = "\033[2m";
    std::string_view italic     = "\033[3m";
    std::string_view underline  = "\033[4m";
    std::string_view red        = "\033[31m";
    std::string_view green      = "\033[32m";
    std::string_view yellow     = "\033[33m";
    std::string_view blue       = "\033[34m";
    std::string_view magenta    = "\033[35m";
    std::string_view cyan       = "\033[36m";
    std::string_view white      = "\033[37m";
    std::string_view bg_dim     = "\033[100m";  // bright black background
};

inline bool is_tty() {
    static bool tty = ::isatty(STDOUT_FILENO) != 0;
    return tty;
}

// ── helpers ──────────────────────────────────────────────────────────

inline std::string trim(std::string_view s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(start, end - start + 1));
}

inline std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty() || (!text.empty() && text.back() == '\n'))
        lines.push_back(line);
    return lines;
}

// Count leading spaces.
inline size_t leading_spaces(std::string_view s) {
    size_t n = 0;
    while (n < s.size() && s[n] == ' ') ++n;
    return n;
}

// ── inline formatting ────────────────────────────────────────────────
// Processes **bold**, *italic*, `code`, [text](url) within a line.

inline std::string render_inline(std::string_view text, const Style& s) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        // Code span: `...`
        if (text[i] == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                if (is_tty()) out += s.bg_dim;
                out += text.substr(i + 1, end - i - 1);
                if (is_tty()) out += s.reset;
                i = end + 1;
                continue;
            }
        }
        // Bold: **...**
        if (i + 2 < text.size() && text[i] == '*' && text[i+1] == '*') {
            size_t end = text.find("**", i + 2);
            if (end != std::string_view::npos) {
                if (is_tty()) out += s.bold;
                out += text.substr(i + 2, end - i - 2);
                if (is_tty()) out += s.reset;
                i = end + 2;
                continue;
            }
        }
        // Italic: *...* (single star, not double)
        if (text[i] == '*' && (i + 1 >= text.size() || text[i+1] != '*')) {
            size_t end = text.find('*', i + 1);
            if (end != std::string_view::npos && (end + 1 >= text.size() || text[end+1] != '*')) {
                if (is_tty()) out += s.italic;
                out += text.substr(i + 1, end - i - 1);
                if (is_tty()) out += s.reset;
                i = end + 1;
                continue;
            }
        }
        // Link: [text](url)
        if (text[i] == '[') {
            size_t close_b = text.find(']', i + 1);
            if (close_b != std::string_view::npos && close_b + 1 < text.size() && text[close_b+1] == '(') {
                size_t close_p = text.find(')', close_b + 2);
                if (close_p != std::string_view::npos) {
                    std::string link_text = std::string(text.substr(i + 1, close_b - i - 1));
                    std::string url = std::string(text.substr(close_b + 2, close_p - close_b - 2));
                    if (is_tty()) out += s.underline;
                    out += link_text;
                    if (is_tty()) out += s.reset;
                    if (is_tty()) out += s.dim;
                    out += " (" + url + ")";
                    if (is_tty()) out += s.reset;
                    i = close_p + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

// ── table rendering ──────────────────────────────────────────────────

struct Table {
    std::vector<std::vector<std::string>> rows;
    size_t cols = 0;
};

// Parse a markdown table into a Table struct.
inline Table parse_table(const std::vector<std::string>& lines, size_t start) {
    Table t;
    size_t i = start;

    // Header row.
    if (i >= lines.size()) return t;
    {
        std::string line = trim(lines[i]);
        if (line.size() < 2 || line.front() != '|' || line.back() != '|') return t;
        line = line.substr(1, line.size() - 2);  // strip outer pipes
        std::vector<std::string> cells;
        std::string cell;
        for (char c : line) {
            if (c == '|') { cells.push_back(trim(cell)); cell.clear(); }
            else cell.push_back(c);
        }
        cells.push_back(trim(cell));
        t.rows.push_back(cells);
        t.cols = cells.size();
        ++i;
    }

    // Separator row (|---|...|) — skip.
    if (i < lines.size()) {
        std::string sep = trim(lines[i]);
        if (sep.size() >= 2 && sep.front() == '|') {
            ++i;  // skip separator
        }
    }

    // Data rows.
    while (i < lines.size()) {
        std::string line = trim(lines[i]);
        if (line.size() < 2 || line.front() != '|' || line.back() != '|') break;
        line = line.substr(1, line.size() - 2);
        std::vector<std::string> cells;
        std::string cell;
        for (char c : line) {
            if (c == '|') { cells.push_back(trim(cell)); cell.clear(); }
            else cell.push_back(c);
        }
        cells.push_back(trim(cell));
        // Pad to match column count.
        while (cells.size() < t.cols) cells.push_back({});
        t.rows.push_back(cells);
        ++i;
    }

    return t;
}

// Render a table with aligned columns.
inline void render_table(const Table& t, const Style& s) {
    if (t.rows.empty() || t.cols == 0) return;

    // Calculate column widths.
    std::vector<size_t> widths(t.cols, 0);
    for (const auto& row : t.rows) {
        for (size_t c = 0; c < row.size() && c < t.cols; ++c) {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    // Terminal width for wrapping.
    int term_width = 80;
    if (is_tty()) {
        struct winsize w;
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
            term_width = w.ws_col;
    }

    // Check if table fits; if not, cap column widths.
    size_t total_width = t.cols + 1;  // pipes + spaces
    for (auto w : widths) total_width += w + 2;  // " w "
    if (total_width > static_cast<size_t>(term_width)) {
        // Shrink proportionally.
        size_t overflow = total_width - term_width;
        for (auto& w : widths) {
            if (overflow > 0 && w > 3) {
                size_t shrink = std::min(w - 3, overflow);
                w -= shrink;
                overflow -= shrink;
            }
        }
    }

    auto render_separator = [&]() {
        if (!is_tty()) return;
        std::cout << s.dim;
        std::cout << '+';
        for (size_t c = 0; c < t.cols; ++c) {
            for (size_t w = 0; w < widths[c] + 2; ++w) std::cout << '-';
            std::cout << '+';
        }
        std::cout << s.reset << '\n';
    };

    auto render_row = [&](const std::vector<std::string>& cells, bool header) {
        std::cout << (is_tty() ? std::string(s.dim) + "|" + std::string(s.reset) : "|");
        for (size_t c = 0; c < t.cols; ++c) {
            std::string cell = c < cells.size() ? cells[c] : "";
            // Truncate if needed.
            if (cell.size() > widths[c]) cell = cell.substr(0, widths[c]);
            std::cout << ' ';
            if (header && is_tty()) std::cout << s.bold;
            std::cout << render_inline(cell, s);
            if (header && is_tty()) std::cout << s.reset;
            // Pad.
            for (size_t p = cell.size(); p < widths[c]; ++p) std::cout << ' ';
            std::cout << ' ';
            std::cout << (is_tty() ? std::string(s.dim) + "|" + std::string(s.reset) : "|");
        }
        std::cout << '\n';
    };

    render_separator();
    // Header.
    if (!t.rows.empty()) {
        render_row(t.rows[0], true);
        render_separator();
    }
    // Data rows.
    for (size_t r = 1; r < t.rows.size(); ++r) {
        render_row(t.rows[r], false);
    }
    render_separator();
}

// ── main render entry point ──────────────────────────────────────────

inline void render(std::string_view text) {
    Style s;
    auto lines = split_lines(text);
    bool in_code_block = false;
    std::string code_lang;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        std::string line = trim(raw);

        // ── Code block ──
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (in_code_block) {
                // End code block.
                in_code_block = false;
                std::cout << '\n';
                continue;
            } else {
                // Start code block.
                in_code_block = true;
                code_lang = line.size() > 3 ? trim(line.substr(3)) : "";
                if (is_tty()) std::cout << s.dim;
                if (!code_lang.empty())
                    std::cout << "``` " << code_lang;
                else
                    std::cout << "```";
                if (is_tty()) std::cout << s.reset;
                std::cout << '\n';
                continue;
            }
        }

        if (in_code_block) {
            // Inside code block — render as-is with dim.
            if (is_tty()) std::cout << s.dim;
            std::cout << raw << '\n';
            if (is_tty()) std::cout << s.reset;
            continue;
        }

        // ── Empty line ──
        if (line.empty()) {
            std::cout << '\n';
            continue;
        }

        // ── Horizontal rule ──
        if (line.size() >= 3 && line.find_first_not_of("-*_") == std::string::npos) {
            if (is_tty()) std::cout << s.dim;
            for (int c = 0; c < 50; ++c) std::cout << '-';
            std::cout << '\n';
            if (is_tty()) std::cout << s.reset;
            continue;
        }

        // ── Heading ──
        if (line[0] == '#') {
            size_t level = 0;
            while (level < line.size() && line[level] == '#') ++level;
            if (level <= 6 && level < line.size() && line[level] == ' ') {
                std::string heading = trim(line.substr(level));
                if (is_tty()) {
                    // Different colors for different levels.
                    std::string color;
                    switch (level) {
                        case 1: color = s.bold; color += s.underline; color += s.cyan; break;
                        case 2: color = s.bold; color += s.cyan; break;
                        case 3: color = s.bold; color += s.blue; break;
                        default: color = s.bold; break;
                    }
                    std::cout << color;
                    // Prefix with # signs.
                    for (size_t h = 0; h < level; ++h) std::cout << '#';
                    std::cout << ' ';
                    std::cout << render_inline(heading, s);
                    std::cout << s.reset;
                } else {
                    for (size_t h = 0; h < level; ++h) std::cout << '#';
                    std::cout << ' ' << heading;
                }
                std::cout << '\n';
                continue;
            }
        }

        // ── Blockquote ──
        if (line[0] == '>') {
            std::string quote = trim(line.substr(1));
            if (is_tty()) std::cout << s.dim << s.italic;
            std::cout << "| " << render_inline(quote, s);
            if (is_tty()) std::cout << s.reset;
            std::cout << '\n';
            continue;
        }

        // ── Table detection ──
        // Check if current line starts a table (starts with |).
        if (line[0] == '|' && i + 1 < lines.size()) {
            // Check next line is separator (|---|).
            std::string next = trim(lines[i + 1]);
            if (next.size() >= 2 && next[0] == '|' && next.find("---") != std::string::npos) {
                Table t = parse_table(lines, i);
                if (!t.rows.empty()) {
                    render_table(t, s);
                    // Skip consumed lines.
                    size_t consumed = t.rows.size() + 1;  // header + separator
                    i += consumed - 1;  // -1 because loop increments
                    continue;
                }
            }
        }

        // ── Unordered list ──
        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line.size() > 1 && line[1] == ' ') {
            std::string item = trim(line.substr(2));
            size_t indent = leading_spaces(raw);
            if (is_tty()) std::cout << s.dim;
            for (size_t sp = 0; sp < indent; ++sp) std::cout << ' ';
            std::cout << "* ";  // bullet
            if (is_tty()) std::cout << s.reset;
            std::cout << render_inline(item, s) << '\n';
            continue;
        }

        // ── Ordered list ──
        if (line.size() > 2 && std::isdigit(line[0])) {
            size_t dot = line.find('.');
            if (dot != std::string::npos && dot + 1 < line.size() && line[dot + 1] == ' ') {
                std::string num = line.substr(0, dot);
                std::string item = trim(line.substr(dot + 2));
                size_t indent = leading_spaces(raw);
                if (is_tty()) std::cout << s.dim;
                for (size_t sp = 0; sp < indent; ++sp) std::cout << ' ';
                std::cout << num << ".";
                if (is_tty()) std::cout << s.reset;
                std::cout << ' ' << render_inline(item, s) << '\n';
                continue;
            }
        }

        // ── Regular paragraph ──
        std::cout << render_inline(raw, s) << '\n';
    }
}

} // namespace markdown
