#include "tinystore/status.h"

#include <cassert>
#include <cerrno>
#include <cstring>

namespace tinystore {

namespace {

// 状态块头部：4 字节消息长度 + 1 字节错误码
constexpr size_t kHeaderSize = 5;

// 分隔两段消息的固定串（例如 "IOError: /data/1.sst: No such file"）
constexpr char kSeparator[] = ": ";
constexpr size_t kSeparatorSize = 2;

}  // namespace

Status::Status(Code code, const Slice& msg, const Slice& msg2)
    : state_(nullptr) {
    assert(code != Code::kOk);  // OK 状态不允许走这条构造路径

    const size_t len1 = msg.size();
    const size_t len2 = msg2.size();
    // 只有第二段非空时才插入 ": " 分隔符
    const size_t msg_size = len1 + (len2 != 0 ? (kSeparatorSize + len2) : 0);

    auto* result = new char[msg_size + kHeaderSize];

    // 用 memcpy 写入 length 而非 reinterpret_cast<uint32_t*>(result) = length：
    // char 数组的对齐要求是 1 字节，直接按 uint32_t 写入会违反严格别名规则
    // （strict aliasing），在 -O2 下可能被优化成错误行为（UBSAN 会报
    // alignment / load of misaligned address）。memcpy 由编译器识别后
    // 同样会优化成单条 mov 指令，性能无损。
    const auto length = static_cast<uint32_t>(msg_size);
    std::memcpy(result, &length, sizeof(length));
    result[4] = static_cast<char>(code);

    char* p = result + kHeaderSize;
    if (len1 != 0) {
        std::memcpy(p, msg.data(), len1);
        p += len1;
    }
    if (len2 != 0) {
        std::memcpy(p, kSeparator, kSeparatorSize);
        p += kSeparatorSize;
        std::memcpy(p, msg2.data(), len2);
    }

    state_ = result;
}

Status::Status(const Status& rhs) : state_(CopyState(rhs.state_)) {}

Status& Status::operator=(const Status& rhs) {
    if (this != &rhs) {
        // 先拷贝再释放：自赋值之外的情况下，如果 new 抛异常，
        // 原对象仍然保持有效状态（强异常安全保证）。
        const char* new_state = CopyState(rhs.state_);
        DeleteState(state_);
        state_ = new_state;
    }
    return *this;
}

Status::Code Status::code() const noexcept {
    if (state_ == nullptr) {
        return Code::kOk;
    }
    // 先转 unsigned char 再转 Code，避免 char 有符号时产生负值
    return static_cast<Code>(static_cast<unsigned char>(state_[4]));
}

Slice Status::message() const {
    if (state_ == nullptr) {
        return Slice("<OK>", 4);
    }
    uint32_t length = 0;
    std::memcpy(&length, state_, sizeof(length));
    return Slice(state_ + kHeaderSize, length);
}

std::string Status::ToString() const {
    if (state_ == nullptr) {
        return "OK";
    }

    const char* type = nullptr;
    switch (code()) {
        case Code::kOk:
            type = "OK";
            break;
        case Code::kNotFound:
            type = "NotFound: ";
            break;
        case Code::kCorruption:
            type = "Corruption: ";
            break;
        case Code::kNotSupported:
            type = "Not implemented: ";
            break;
        case Code::kInvalidArgument:
            type = "Invalid argument: ";
            break;
        case Code::kIOError:
            type = "IO error: ";
            break;
    }

    const Slice msg = message();
    std::string result(type);
    result.append(msg.data(), msg.size());
    return result;
}

const char* Status::CopyState(const char* state) {
    if (state == nullptr) {
        return nullptr;
    }
    uint32_t length = 0;
    std::memcpy(&length, state, sizeof(length));
    const size_t size = static_cast<size_t>(length) + kHeaderSize;

    auto* result = new char[size];
    std::memcpy(result, state, size);
    return result;
}

void Status::DeleteState(const char* state) noexcept { delete[] state; }

Status Status::NotFound(const Slice& msg, const Slice& msg2) {
    return Status(Code::kNotFound, msg, msg2);
}
Status Status::Corruption(const Slice& msg, const Slice& msg2) {
    return Status(Code::kCorruption, msg, msg2);
}
Status Status::NotSupported(const Slice& msg, const Slice& msg2) {
    return Status(Code::kNotSupported, msg, msg2);
}
Status Status::InvalidArgument(const Slice& msg, const Slice& msg2) {
    return Status(Code::kInvalidArgument, msg, msg2);
}
Status Status::IOError(const Slice& msg, const Slice& msg2) {
    return Status(Code::kIOError, msg, msg2);
}

Status IOErrorFromErrno(const Slice& context, int err_number) {
    // 这里必须用 strerror_r 的线程安全版本。
    // strerror() 返回指向静态缓冲区的指针，多线程下会被其他线程覆盖 ——
    // 存储引擎有后台 Compaction 线程，这种 bug 是真实存在的。
    //
    // GNU 版本的 strerror_r 返回 char*（不一定用提供的缓冲区），
    // XSI 版本返回 int。两者签名不兼容，需要靠宏区分。
    char buf[256];
    buf[0] = '\0';

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    // GNU 变体：返回指向消息的指针，可能指向 buf，也可能指向静态字符串
    const char* msg = ::strerror_r(err_number, buf, sizeof(buf));
    return Status::IOError(context, Slice(msg));
#else
    // XSI 变体：成功返回 0，消息写入 buf
    if (::strerror_r(err_number, buf, sizeof(buf)) == 0) {
        return Status::IOError(context, Slice(buf));
    }
    return Status::IOError(context, Slice("unknown error"));
#endif
}

}  // namespace tinystore
