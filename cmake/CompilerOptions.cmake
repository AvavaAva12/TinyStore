# ---------------------------------------------------------------------------
# 集中管理编译选项
#
# 设计动机：把编译选项挂在 INTERFACE 库（tinystore_options）上，而不是用全局的
# add_compile_options()。这样做的好处是：
#   1. 选项随目标传播 —— 任何链接 tinystore 的目标自动继承，不会漏配；
#   2. 不污染全局 —— 将来引入第三方依赖时，第三方代码不会被强制套上我们的严格警告；
#   3. 便于切换 —— 只需改一处即可换编译器或调整警告等级。
# ---------------------------------------------------------------------------

add_library(tinystore_options INTERFACE)

target_compile_features(tinystore_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(tinystore_options INTERFACE
        /W4
        /permissive-
        /Zc:__cplusplus
    )
    target_compile_definitions(tinystore_options INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
else()
    target_compile_options(tinystore_options INTERFACE
        -Wall                  # 启用常用的高价值警告
        -Wextra                # 在 -Wall 基础上补充更多检查
        -Wpedantic             # 拒绝非标准扩展，保证可移植性
        -Wshadow               # 变量遮蔽：系统级代码里极易埋雷
        -Wnon-virtual-dtor     # 有虚函数却没有虚析构 -> 通过基类指针 delete 会 UB
        -Woverloaded-virtual   # 派生类意外隐藏（而非重写）基类虚函数
        -Wcast-qual            # 去掉 const 限定符的强制转换
        -Wformat=2             # printf 风格格式化串检查
        -Wnull-dereference     # 静态可判定的空指针解引用
        -fno-omit-frame-pointer  # perf 采样需要完整的调用栈
    )

    # Release / RelWithDebInfo 下保留帧指针，否则 perf report 里的调用栈是断的。
    target_compile_options(tinystore_options INTERFACE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-fno-omit-frame-pointer>
    )
endif()

# ---------------------------------------------------------------------------
# 严格警告（可选）
#
# -Wconversion / -Wsign-conversion 能抓出大量隐式整型转换 bug，在存储引擎里
# （offset、size、sequence number 到处混用 32/64 位）尤其有价值。
# 但它噪音较大，因此做成可选开关：日常开发关闭，提交前打开做一次专项清理。
# ---------------------------------------------------------------------------
option(TINYSTORE_STRICT_WARNINGS "Enable noisy conversion warnings" OFF)

if(TINYSTORE_STRICT_WARNINGS AND NOT MSVC)
    target_compile_options(tinystore_options INTERFACE
        -Wconversion
        -Wsign-conversion
        -Wold-style-cast
        -Wuseless-cast
    )
endif()
