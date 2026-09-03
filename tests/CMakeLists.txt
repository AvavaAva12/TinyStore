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
