# ---------------------------------------------------------------------------
# Sanitizer 配置
#
# 为什么第一周就要把 Sanitizer 配好？
#   ASAN  能抓出越界访问、use-after-free、内存泄漏 —— 写 Arena / Slice 这类
#         手动管理内存的代码时，越界往往不会立刻崩溃，而是静默破坏相邻数据，
#         等到几万次操作后才表现成"数据损坏"，排查成本极高。
#   TSAN  能抓出 data race —— 等到 W8 引入后台 Compaction 线程后再上就太晚了，
#         那时代码量大、竞争点分散，定位困难。
#   UBSAN 能抓出未定义行为（有符号溢出、对齐错误、非法的枚举值等）。
#
# 使用方式：
#   cmake -B build-asan -DTINYSTORE_ENABLE_ASAN=ON && cmake --build build-asan
#   ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
#
# 注意：ASAN 与 TSAN 互斥，不能同时开启（两者都 hook 了内存分配器）。
# ---------------------------------------------------------------------------

option(TINYSTORE_ENABLE_ASAN  "Enable AddressSanitizer"           OFF)
option(TINYSTORE_ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)
option(TINYSTORE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(TINYSTORE_ENABLE_ASAN AND TINYSTORE_ENABLE_TSAN)
    message(FATAL_ERROR
        "ASAN and TSAN are mutually exclusive; enable only one at a time.")
endif()

if(MSVC)
    if(TINYSTORE_ENABLE_ASAN)
        target_compile_options(tinystore_options INTERFACE /fsanitize=address)
    endif()
    return()
endif()

# -fno-omit-frame-pointer + -g：让 Sanitizer 的报错带上可读的调用栈。
# -fno-optimize-sibling-calls：防止尾调用优化把栈帧折叠掉，导致栈回溯缺层。
set(_san_common_flags -fno-omit-frame-pointer -fno-optimize-sibling-calls)

if(TINYSTORE_ENABLE_ASAN)
    message(STATUS "Building with AddressSanitizer")
    target_compile_options(tinystore_options INTERFACE
        -fsanitize=address
        -fsanitize-address-use-after-scope
        ${_san_common_flags})
    target_link_options(tinystore_options INTERFACE -fsanitize=address)
endif()

if(TINYSTORE_ENABLE_TSAN)
    message(STATUS "Building with ThreadSanitizer")
    target_compile_options(tinystore_options INTERFACE
        -fsanitize=thread ${_san_common_flags})
    target_link_options(tinystore_options INTERFACE -fsanitize=thread)
endif()

if(TINYSTORE_ENABLE_UBSAN)
    message(STATUS "Building with UndefinedBehaviorSanitizer")
    target_compile_options(tinystore_options INTERFACE
        -fsanitize=undefined
        -fsanitize=float-divide-by-zero
        -fno-sanitize-recover=all   # 遇到 UB 直接中止，而不是继续带着错误状态跑
        ${_san_common_flags})
    target_link_options(tinystore_options INTERFACE -fsanitize=undefined)
endif()
