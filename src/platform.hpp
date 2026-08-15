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

#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
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
#else
  #include <termios.h>
  #include <unistd.h>
#endif

namespace platform {

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
// localtime_r (POSIX/MinGW) vs localtime_s (MSVC, reversed arg order).
inline void localtime_portable(const std::time_t* t, std::tm* out) {
#if defined(_WIN32) && defined(_MSC_VER)
    localtime_s(out, t);   // MSVC: (dest, src)
#else
    localtime_r(t, out);   // POSIX + MinGW-w64
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

} // namespace platform
