cmake_minimum_required(VERSION 3.16)
project(cpp_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Termux detection: the $PREFIX env var is set by Termux to the userland prefix
# (typically /data/data/com.termux/files/usr). The toolchain also defines
# __ANDROID__ / __BIONIC__, but $PREFIX is available at configure time and is
# what pkg uses, so we use it for search-path setup.
if(DEFINED ENV{PREFIX} AND NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "$ENV{PREFIX}")
    list(APPEND CMAKE_LIBRARY_PATH "$ENV{PREFIX}/lib")
    list(APPEND CMAKE_INCLUDE_PATH "$ENV{PREFIX}/include")
    message(STATUS "Termux prefix detected: $ENV{PREFIX}")
    set(TERMUX_BUILD ON)
endif()

# On Termux, clang++ defaults to libc++; on desktop clang it usually defaults to
# libstdc++. Probe libc++ once and prefer it if usable so the build matches the
# Termux ABI regardless of host.
include(CheckCXXSourceCompiles)
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

# Help locate shared libs at runtime on Termux (loader already searches
# $PREFIX/lib, but be explicit in case of cross-prefix installs).
if(TERMUX_BUILD)
    set_target_properties(coding-agent PROPERTIES
        BUILD_RPATH "$ENV{PREFIX}/lib"
        INSTALL_RPATH "$ENV{PREFIX}/lib")
endif()
