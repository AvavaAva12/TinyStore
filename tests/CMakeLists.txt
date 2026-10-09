find_package(GTest REQUIRED)

# ---------------------------------------------------------------------------
# TSAN 在 WSL2 上的启动包装
#
# TSAN 要求进程地址空间落在特定范围内，与 WSL2 内核的地址随机化冲突，
# 直接运行会报 "FATAL: ThreadSanitizer: unexpected memory mapping"。
# 用 `setarch -R` 关闭单个进程的 ASLR 即可绕过（详见 docs/design-notes.md）。
#
# 这里把它直接包进 add_test 的命令里，使得
# `ctest --test-dir build-tsan` 开箱即用，无需手工 setarch。
# ---------------------------------------------------------------------------
set(TINYSTORE_TEST_PREFIX "")
if(TINYSTORE_ENABLE_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(TINYSTORE_SETARCH NAMES setarch)
    if(TINYSTORE_SETARCH)
        message(STATUS "TSAN detected: wrapping tests with ${TINYSTORE_SETARCH} -R")
        set(TINYSTORE_TEST_PREFIX ${TINYSTORE_SETARCH} -R)
    else()
        message(WARNING
            "TSAN enabled but setarch not found; tests may fail to start.")
    endif()
endif()

# ---------------------------------------------------------------------------
# 统一的测试目标注册函数
#
# 每个测试文件编译成一个独立的可执行文件，而不是全部链接进一个大 binary。
# 好处：
#   * 单个测试崩溃不会掩盖其他测试的结果；
#   * 可以只运行一个用例（ctest -R arena_test），迭代更快；
#   * 将来接入 Sanitizer 时，可以只对特定模块开启。
# ---------------------------------------------------------------------------
function(tinystore_add_test name)
    add_executable(${name} ${name}.cpp)

    target_link_libraries(${name}
        PRIVATE
            tinystore
            GTest::gtest
            GTest::gtest_main
            tinystore_options
    )

    # 让测试可执行文件在 build/bin 下，ctest 能直接找到
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    if(TINYSTORE_TEST_PREFIX)
        add_test(NAME ${name}
                 COMMAND ${TINYSTORE_TEST_PREFIX} $<TARGET_FILE:${name}>)
    else()
        add_test(NAME ${name} COMMAND ${name})
    endif()
endfunction()

tinystore_add_test(slice_test)
tinystore_add_test(coding_test)
tinystore_add_test(status_test)
tinystore_add_test(comparator_test)
tinystore_add_test(arena_test)
tinystore_add_test(internal_key_test)
tinystore_add_test(env_test)
tinystore_add_test(crc32c_test)
tinystore_add_test(skiplist_test)
tinystore_add_test(memtable_test)
tinystore_add_test(write_batch_test)
tinystore_add_test(log_test)
tinystore_add_test(db_test)
tinystore_add_test(table_test)
tinystore_add_test(filter_test)
tinystore_add_test(iterator_test)

# ---------------------------------------------------------------------------
# 崩溃恢复测试
#
# 【为什么单独注册而不在 TSAN 下运行】
# 它用 fork 制造"进程被杀"的场景。TSAN 无法跨 fork 跟踪内存访问：子进程里
# 只有当前线程存在，TSAN 会把父进程遗留的所有分配与锁状态判为异常，
# 产生大量与被测代码无关的误报。
# 这不是掩盖问题：fork 本身被 TSAN 列为不支持的场景，且本测试验证的是
# **持久化协议**（文件与 MANIFEST 的先后顺序），不依赖内存序，天然无竞争可查。
# 其余三类竞争（读路径无锁、后台 compaction、Group Commit）已由 db_test
# 在 TSAN 下覆盖。
# ---------------------------------------------------------------------------
if(NOT TINYSTORE_ENABLE_TSAN)
    # 不复用 tinystore_add_test：本文件自带 main（要用同一个二进制以子进程身份
    # fork+exec 自己），因此只链接 gtest 而不链接提供 main 的 gtest_main。
    add_executable(crash_test crash_test.cpp)
    target_link_libraries(crash_test
        PRIVATE tinystore GTest::gtest tinystore_options)
    set_target_properties(crash_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    if(TINYSTORE_TEST_PREFIX)
        add_test(NAME crash_test
                 COMMAND ${TINYSTORE_TEST_PREFIX} $<TARGET_FILE:crash_test>)
    else()
        add_test(NAME crash_test COMMAND crash_test)
    endif()
else()
    message(STATUS "TSAN detected: skipping crash_test (fork is unsupported by TSAN)")
endif()
