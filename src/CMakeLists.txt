# ---------------------------------------------------------------------------
# 核心库
#
# 目前是单体库 tinystore。随着模块增多（WAL / SSTable / Compaction / 网络层），
# 会保持在同一库内分目录组织，而不是拆成多个 .a —— 理由：
#   1. 单机嵌入式库拆库没有部署收益，反而增加符号可见性和链接顺序的心智负担；
#   2. 保持单一库便于编译器做跨模块内联（未来可配合 LTO 进一步优化）。
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)

add_library(tinystore STATIC
    status.cpp
    comparator.cpp
    arena.cpp
    internal_key.cpp
    env_posix.cpp
    crc32c.cpp
    memtable.cpp
    write_batch.cpp
    log_writer.cpp
    log_reader.cpp
)

target_include_directories(tinystore
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(tinystore
    PUBLIC
        tinystore_options
        Threads::Threads
)

set_target_properties(tinystore PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 生成 compile_commands.json，供 clangd / clang-tidy 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
if(CMAKE_EXPORT_COMPILE_COMMANDS AND EXISTS ${CMAKE_BINARY_DIR}/compile_commands.json)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${PROJECT_SOURCE_DIR}/compile_commands.json
        OUTPUT_QUIET ERROR_QUIET
    )
endif()
