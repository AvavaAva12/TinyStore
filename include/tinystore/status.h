#pragma once

#include <cstdint>
#include <string>

#include "tinystore/slice.h"

namespace tinystore {

// ===========================================================================
// Status —— 函数返回的错误状态
// ===========================================================================
//
// 【为什么不用 C++ 异常】
// 这是底层系统库的经典取舍，面试高频问题。不用的理由有四条：
//
//   1. 性能模型不可预测。异常在本项目这种"每个操作都要检查错误"的代码里，
//      如果打开异常，编译器需要为每条可能抛出的路径生成 unwind 表，
//      二进制体积和指令缓存占用都会上升。而 Status 只是一次指针判空。
//
//   2. 错误是"常态"而非"异常"。存储引擎里 "key 不存在"（NotFound）是
//      完全正常的业务结果，用异常表达正常控制流在语义上就是错的。
//
//   3. 与 C API / 系统调用的边界一致。read/write/pread 都用返回码 + errno
//      表达错误，用 Status 可以自然地把 errno 包进来，不用做异常转换。
//
//   4. 便于强制检查。配合 [[nodiscard]]，编译器能在调用方漏掉错误检查时报警。
//      （本项目为了代码可读性，把 [[nodiscard]] 放在了工厂函数上。）
//
// 【内存布局：为什么不用 std::string 成员】
// Status 的大小直接影响所有接口的调用开销。用裸 char* 布局：
//
//     state_ == nullptr                    -> 表示 OK（零成本）
//     state_ -> [u32 msg长度][u8 错误码][msg 字节...]
//
// 好处：
//   * OK 状态无需任何分配，sizeof(Status) == sizeof(void*) == 8 字节，
//     可以通过寄存器返回，不需要栈上构造临时对象；
//   * 错误路径才分配内存，属于慢路径，分配开销可以接受。
//
// 代价：需要手写拷贝/移动语义（Rule of Five）。这正是本项目想练习的地方。
// ===========================================================================
class Status {
public:
    // 错误码用 uint8_t 而非 int：布局里只留了 1 字节存它。
    // 用 enum class 保证不会和整数隐式混用。
    enum class Code : uint8_t {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,      // 数据损坏（校验和不匹配、格式非法）
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5,         // 系统调用失败，原始 errno 会附在消息里
    };

    // OK 状态：state_ == nullptr
    Status() noexcept : state_(nullptr) {}

    ~Status() { DeleteState(state_); }

    Status(const Status& rhs);
    Status& operator=(const Status& rhs);

    Status(Status&& rhs) noexcept : state_(rhs.state_) { rhs.state_ = nullptr; }

    Status& operator=(Status&& rhs) noexcept {
        if (this != &rhs) {
            DeleteState(state_);
            state_ = rhs.state_;
            rhs.state_ = nullptr;
        }
        return *this;
    }

    // --- 工厂函数 ---
    [[nodiscard]] static Status OK() { return Status(); }

    [[nodiscard]] static Status NotFound(const Slice& msg,
                                         const Slice& msg2 = Slice());
    [[nodiscard]] static Status Corruption(const Slice& msg,
                                           const Slice& msg2 = Slice());
    [[nodiscard]] static Status NotSupported(const Slice& msg,
                                             const Slice& msg2 = Slice());
    [[nodiscard]] static Status InvalidArgument(const Slice& msg,
                                                const Slice& msg2 = Slice());
    [[nodiscard]] static Status IOError(const Slice& msg,
                                        const Slice& msg2 = Slice());

    bool ok() const noexcept { return state_ == nullptr; }

    bool IsNotFound() const noexcept { return code() == Code::kNotFound; }
    bool IsCorruption() const noexcept { return code() == Code::kCorruption; }
    bool IsNotSupported() const noexcept {
        return code() == Code::kNotSupported;
    }
    bool IsInvalidArgument() const noexcept {
        return code() == Code::kInvalidArgument;
    }
    bool IsIOError() const noexcept { return code() == Code::kIOError; }

    Code code() const noexcept;

    // 返回的 Slice 指向 Status 内部缓冲区，
    // 调用方必须保证 Status 的生命周期长于该 Slice。
    Slice message() const;

    // 形如 "NotFound: key=xxx" 的完整可读描述
    std::string ToString() const;

private:
    Status(Code code, const Slice& msg, const Slice& msg2);

    static const char* CopyState(const char* state);
    static void DeleteState(const char* state) noexcept;

    // 布局: [u32 length][u8 code][msg...]，OK 时为 nullptr
    const char* state_;
};

// 便于把系统调用的返回值转成 Status：
//     if (::fsync(fd) != 0) return IOErrorFromErrno("fsync", errno);
Status IOErrorFromErrno(const Slice& context, int err_number);

}  // namespace tinystore
