cmake_minimum_required(VERSION 3.16)
project(cpp_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Static build option ─────────────────────────────────────────────
# Enable with: cmake -DBUILD_STATIC=ON ...
# Produces a statically-linked binary (no shared library deps).
# NOTE: On Termux/Android (Bionic libc), full static linking is not
# supported by the platform — libc++ ships only as a shared library.
# The option will still try to statically link third-party libs (curl)
# while keeping system libraries dynamic.
option(BUILD_STATIC "Build a static executable (no shared library deps)" OFF)

# ── Platform detection ──────────────────────────────────────────────
# Termux runs on Android with Bionic libc, which defines __ANDROID__.
# Regular ARM64 Linux uses glibc/musl and does NOT define __ANDROID__.
# This compile-time probe is more robust than checking $PREFIX.
include(CheckCXXSourceCompiles)
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
endif()

# On Termux, clang++ defaults to libc++; on desktop clang it usually defaults to
# libstdc++. Probe libc++ once and prefer it if usable so the build matches the
# Termux ABI regardless of host.
set(CMAKE_REQUIRED_FLAGS "-stdlib=libc++")
check_cxx_source_compiles("#include <string>\nint main(){return 0;}" HAVE_LIBCXX_STDLIB)
unset(CMAKE_REQUIRED_FLAGS)
if(HAVE_LIBCXX_STDLIB)
    message(STATUS "Using libc++ runtime")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

# On bionic (Termux/Android) libpthread is part of libc, but the -pthread flag
# is still the portable way to request threading support on Linux/BSD.
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options(-pthread)
add_link_options(-pthread)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(coding-agent
    src/main.cpp
)
target_include_directories(coding-agent PRIVATE src)
target_link_libraries(coding-agent PRIVATE CURL::libcurl Threads::Threads)

# ── Static build configuration ──────────────────────────────────────
# Applied after target creation so target_link_options can reference it.
if(BUILD_STATIC)
    if(TERMUX_BUILD)
        # Bionic libc does NOT support full -static linking, and Termux
        # ships libc++ as a shared library (libc++_shared.so) with no
        # static counterpart. The best we can do is statically link
        # third-party libraries (curl, etc.) while keeping system libs
        # dynamic.
        message(STATUS "Termux detected: attempting to statically link "
                       "third-party libraries (system libs stay dynamic)")

        # Try to locate and use static libcurl
        find_library(CURL_STATIC NAMES libcurl.a
                     PATHS "$ENV{PREFIX}/lib"
                     NO_DEFAULT_PATH
                     NO_CMAKE_FIND_ROOT_PATH)
        if(CURL_STATIC AND CURL_STATIC MATCHES "\.a$")
            message(STATUS "Found static libcurl: ${CURL_STATIC}")
            target_link_libraries(coding-agent PRIVATE "${CURL_STATIC}")
        else()
            message(WARNING "libcurl.a not found in $ENV{PREFIX}/lib — "
                            "curl will be dynamically linked. "
                            "Install static curl with: pkg install libcurl-static "
                            "(if available) or build curl from source.")
        endif()
    else()
        # Desktop Linux / glibc: full static linking is supported.
        message(STATUS "Desktop Linux: using full static linking")
        target_link_options(coding-agent PRIVATE -static -static-libgcc -static-libstdc++)
        # Prefer static libs for further find_package calls
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;${CMAKE_FIND_LIBRARY_SUFFIXES}")
    endif()
endif()

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
