cmake_minimum_required(VERSION 3.20)

project(TinyStore
    VERSION 0.1.0
    DESCRIPTION "A LSM-Tree storage engine written from scratch for learning systems programming"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局 C++ 标准设置
# ---------------------------------------------------------------------------
# 使用 C++20 而非更高的标准：C++20 已被主流编译器（GCC 13 / Clang 18）完整支持，
# 且足以覆盖本项目需要的 language features（concepts、std::span、原子操作等）。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # 禁用 -std=gnu++20，保证代码的可移植性

# 未显式指定 CMAKE_BUILD_TYPE 时默认使用 RelWithDebInfo：
# 带优化（-O2）又保留调试符号（-g），便于用 perf / gdb 做性能剖析。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# 把编译产物统一放到 build/ 下，避免污染源码树
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(CompilerOptions)
include(Sanitizers)

# ---------------------------------------------------------------------------
# 构建开关
# ---------------------------------------------------------------------------
option(TINYSTORE_BUILD_TESTS      "Build unit tests"          ON)
option(TINYSTORE_BUILD_BENCHMARKS "Build micro benchmarks"    ON)

# ---------------------------------------------------------------------------
# 子目录
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(TINYSTORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TINYSTORE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
