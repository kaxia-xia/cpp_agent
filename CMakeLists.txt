cmake_minimum_required(VERSION 3.16)
project(cpp_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Platform detection ──────────────────────────────────────────────
# Termux runs on Android with Bionic libc, which defines __ANDROID__.
# Regular Linux (glibc/musl) does NOT define __ANDROID__.
# Windows is detected via CMake's built-in WIN32 variable.
include(CheckCXXSourceCompiles)

if(WIN32)
    set(IS_ANDROID FALSE)
    set(TERMUX_BUILD OFF)
    message(STATUS "Windows platform detected")
else()
    check_cxx_source_compiles("#ifndef __ANDROID__\n#error not android\n#endif\nint main(){return 0;}" IS_ANDROID)
    if(IS_ANDROID)
        message(STATUS "Android/Termux platform detected (__ANDROID__)")
        set(TERMUX_BUILD ON)

        # On Termux, pkg installs everything under $PREFIX.
        if(DEFINED ENV{PREFIX})
            set(CMAKE_PREFIX_PATH "$ENV{PREFIX}")
            list(APPEND CMAKE_LIBRARY_PATH "$ENV{PREFIX}/lib")
            list(APPEND CMAKE_INCLUDE_PATH "$ENV{PREFIX}/include")
            message(STATUS "Termux prefix: $ENV{PREFIX}")
        endif()
    else()
        message(STATUS "Generic Linux platform detected (no __ANDROID__)")
        set(TERMUX_BUILD OFF)
    endif()
endif()

# On Termux, clang++ defaults to libc++; on desktop clang it usually defaults to
# libstdc++. Probe libc++ once and prefer it if usable so the build matches the
# Termux ABI regardless of host.  (GCC/Clang only — MSVC has its own runtime.)
if(NOT MSVC)
    set(CMAKE_REQUIRED_FLAGS "-stdlib=libc++")
    check_cxx_source_compiles("#include <string>\nint main(){return 0;}" HAVE_LIBCXX_STDLIB)
    unset(CMAKE_REQUIRED_FLAGS)
    if(HAVE_LIBCXX_STDLIB)
        message(STATUS "Using libc++ runtime")
        add_compile_options(-stdlib=libc++)
        add_link_options(-stdlib=libc++)
    endif()
endif()

# Warning flags: GCC/Clang vs MSVC.
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# On bionic (Termux/Android) libpthread is part of libc, but the -pthread flag
# is still the portable way to request threading support on Linux/BSD.
# Windows threading is handled by find_package(Threads) below.
if(NOT WIN32)
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(coding-agent
    src/main.cpp
)
target_include_directories(coding-agent PRIVATE src)
target_link_libraries(coding-agent PRIVATE CURL::libcurl Threads::Threads)

# Help locate shared libs at runtime on Termux (loader already searches
# $PREFIX/lib, but be explicit in case of cross-prefix installs).
if(TERMUX_BUILD)
    set_target_properties(coding-agent PROPERTIES
        BUILD_RPATH "$ENV{PREFIX}/lib"
        INSTALL_RPATH "$ENV{PREFIX}/lib")
endif()

# ── System prompt file ──────────────────────────────────────────────
# Copy the platform-appropriate system prompt to the build directory.
# The executable reads system_prompt.txt from its own directory at runtime.
# Uses add_custom_command(OUTPUT ...) instead of POST_BUILD so CMake
# tracks the source file as a dependency — editing the prompt will
# trigger a copy even when the main binary doesn't need recompiling.
if(TERMUX_BUILD)
    set(PROMPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/system_prompt_termux.txt")
elseif(WIN32)
    set(PROMPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/system_prompt_windows.txt")
else()
    set(PROMPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/system_prompt_linux.txt")
endif()
set(PROMPT_DEST "${CMAKE_CURRENT_BINARY_DIR}/system_prompt.txt")
add_custom_command(
    OUTPUT  "${PROMPT_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROMPT_SOURCE}" "${PROMPT_DEST}"
    DEPENDS "${PROMPT_SOURCE}"
    COMMENT "Copying system_prompt.txt (TERMUX_BUILD=${TERMUX_BUILD})"
)
add_custom_target(copy_prompt DEPENDS "${PROMPT_DEST}")
add_dependencies(coding-agent copy_prompt)

# ── Install ─────────────────────────────────────────────────────────
# One-click install: cmake --install build [--prefix /path]
# Default prefix: ~/.local  →  installs to ~/.local/bin/
# Override examples:
#   cmake --install build --prefix /usr/local
#   cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/coding-agent ..
if(NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # User explicitly set -DCMAKE_INSTALL_PREFIX, keep it
elseif(WIN32)
    set(CMAKE_INSTALL_PREFIX "$ENV{USERPROFILE}/.local" CACHE PATH
        "Install prefix (binary goes to <prefix>/bin)" FORCE)
else()
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH
        "Install prefix (binary goes to <prefix>/bin)" FORCE)
endif()
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}  (binary → ${CMAKE_INSTALL_PREFIX}/bin)")

install(TARGETS coding-agent
    RUNTIME DESTINATION "bin"
)
install(FILES "${PROMPT_DEST}"
    DESTINATION "bin"
    RENAME "system_prompt.txt"
)
