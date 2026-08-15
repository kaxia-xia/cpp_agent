// platform.hpp - small cross-platform helpers (TTY, signals, exe dir, time).
//
// Everything platform-specific that the rest of the code needs is
// centralised here so the higher-level modules stay clean.
//
//   Linux (glibc/musl)   -> POSIX path
//   Android (Termux)     -> POSIX path (Bionic)
//   Windows x64          -> Win32 path (MSVC or MinGW-w64)
//
#pragma once

#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>

// ── Windows guard: keep windows.h lean and avoid macro pollution ────
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <io.h>

  // Older MinGW headers may not define the virtual-terminal flag.
  #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
    #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
  #endif
#else
  #include <sys/ioctl.h>
  #include <termios.h>
  #include <unistd.h>
#endif

namespace platform {

// ── ANSI / console capability ───────────────────────────────────────
// On Windows the console must have UTF-8 code page and VT processing
// enabled before ANSI escapes render correctly; otherwise the raw
// sequences (e.g. "35m") leak through and Chinese text mojibakes.
inline bool& ansi_available_flag() {
    static bool v = true;
    return v;
}

inline bool ansi_supported() {
    return ansi_available_flag();
}

// Must be called once at startup, before any output.
inline void setup_console() {
#ifdef _WIN32
    // 1) UTF-8 code page — fixes Chinese mojibake in PowerShell/cmd.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 2) Enable ANSI/VT escape processing on stdout when it is a console.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr &&
        GetConsoleMode(hOut, &mode)) {
        mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        ansi_available_flag() = (SetConsoleMode(hOut, mode) != 0);
    } else {
        ansi_available_flag() = false;   // redirected: no ANSI
    }
#else
    ansi_available_flag() = (::isatty(STDOUT_FILENO) != 0);
#endif
}

// ── TTY detection ────────────────────────────────────────────────────
inline bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

inline bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

// ── UTF-8 console line input ─────────────────────────────────────────
// Windows: narrow stdin reads (std::cin / ReadConsoleA) are unreliable
// when the console input code page is CP_UTF8 — conhost drops or mangles
// multi-byte (non-ASCII) characters, so Chinese text typed in the REPL
// can be reduced to its ASCII fragments only.  We therefore read the
// console as wide characters via ReadConsoleW and convert to UTF-8
// ourselves.  For redirected (non-console) stdin we fall back to the
// ordinary byte-stream read.
inline bool read_console_line_utf8(std::string& line) {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(hIn, &mode)) {
            std::wstring wline;
            wchar_t buf[4096];
            DWORD read = 0;
            while (true) {
                if (!ReadConsoleW(hIn, buf, 4096, &read, nullptr)) {
                    return false;   // console closed / read error
                }
                if (read == 0) {
                    return false;
                }
                wline.append(buf, read);
                if (!wline.empty() && wline.back() == L'\n') break;
            }
            // Strip trailing CR/LF.
            while (!wline.empty() &&
                   (wline.back() == L'\n' || wline.back() == L'\r')) {
                wline.pop_back();
            }
            // Windows console EOF marker (Ctrl+Z) → treat as end-of-input.
            if (wline.size() == 1 && wline[0] == L'\x1A') {
                return false;
            }
            if (wline.empty()) {
                line.clear();
                return true;
            }
            int n = WideCharToMultiByte(CP_UTF8, 0, wline.data(),
                                        static_cast<int>(wline.size()),
                                        nullptr, 0, nullptr, nullptr);
            if (n <= 0) {
                line.clear();
                return true;
            }
            line.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, wline.data(),
                                static_cast<int>(wline.size()),
                                line.data(), n, nullptr, nullptr);
            return true;
        }
    }
    // Not an interactive console (stdin redirected / piped).
    return bool(std::getline(std::cin, line));
#else
    // POSIX: stdin already delivers UTF-8 bytes; just read a line.
    return bool(std::getline(std::cin, line));
#endif
}

// ── Directory containing the running executable ──────────────────────
// Linux uses /proc/self/exe; Windows uses GetModuleFileNameA.
inline std::filesystem::path exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(std::string(buf, n)).parent_path();
    }
    return std::filesystem::current_path();
#else
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe.parent_path();
    return std::filesystem::current_path();
#endif
}

// ── Portable localtime ───────────────────────────────────────────────
// localtime_s (Windows, MSVC + MinGW-w64) vs localtime_r (POSIX).
inline void localtime_portable(const std::time_t* t, std::tm* out) {
#ifdef _WIN32
    localtime_s(out, t);   // Windows: (dest, src)
#else
    localtime_r(t, out);   // POSIX
#endif
}

// ── SIGINT installation ──────────────────────────────────────────────
// POSIX uses sigaction; Windows only has signal().  Both take a
// `void(*)(int)` handler, so the caller-side code stays identical.
inline void install_sigint(void (*handler)(int)) {
#ifdef _WIN32
    std::signal(SIGINT, handler);
#else
    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
#endif
}

// ── Terminal flow control (Ctrl+S / Ctrl+Q) ──────────────────────────
// POSIX: enable kernel IXON flow control.
// Windows: no equivalent → report unavailable.
inline bool enable_flow_control() {
#ifdef _WIN32
    return false;
#else
    if (!stdin_is_tty()) return false;
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return false;
    if (t.c_iflag & IXON) return true;   // already enabled
    t.c_iflag |= IXON;
    t.c_cc[VSTART] = 0x11;   // Ctrl+Q
    t.c_cc[VSTOP]  = 0x13;   // Ctrl+S
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
    return true;
#endif
}

// ── Terminal width (columns) ────────────────────────────────────────
// POSIX: TIOCGWINSZ ioctl.  Windows: GetConsoleScreenBufferInfo.
inline int terminal_width(int fallback = 80) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != nullptr &&
        GetConsoleScreenBufferInfo(h, &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (w > 0) return w;
    }
    return fallback;
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return static_cast<int>(ws.ws_col);
    return fallback;
#endif
}

// ── Format current local time (strftime) ────────────────────────────
inline std::string format_time_now(const std::string& fmt) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_portable(&t, &tm);
    char buf[512];
    if (std::strftime(buf, sizeof(buf), fmt.c_str(), &tm) == 0) return {};
    return std::string(buf);
}

// ── Current Unix timestamp (seconds) ────────────────────────────────
inline long long unix_timestamp_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace platform
