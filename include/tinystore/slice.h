#pragma once

#include <cstddef>
#include <cstring>
#include <ostream>
#include <string>

namespace tinystore {

// ===========================================================================
// Slice —— 一段"非拥有"（non-owning）的只读字节区间
// ===========================================================================
//
// 【为什么需要它】
// 存储引擎的热路径上到处都要传递 key / value。如果统一用 std::string 按值传递：
//   * 每次传参都可能触发一次堆分配 + 一次 memcpy（对小字符串有 SSO 优化，但
//     一旦超过 15 字节就退化）；
//   * 从 mmap 或 read 缓冲区里取一段数据时，明明已经有内存了，却还要再拷一份。
// Slice 只是一对 (pointer, length)，拷贝成本等于拷贝两个机器字，且不涉及所有权。
//
// 【为什么不用 std::string_view】
// std::string_view 在语义上几乎等价，完全可以替代。本项目手写一遍的原因是：
//   1. 它是理解"悬垂引用"最好的教材 —— Slice 不延长被指对象的生命周期，
//      你必须自己保证底层 buffer 活得比 Slice 久（见 MemTable / Block 的设计）；
//   2. 我们需要自定义比较语义（compare 用 memcmp，而非字典序的元素比较），
//      并且后续要在 Slice 上挂 CRC、序列化等配套工具函数。
// 生产环境直接用 std::string_view 即可，这里的重复实现纯粹是教学目的。
//
// 【核心约束（务必牢记）】
// Slice 不拥有数据。下面这样写是悬垂的：
//     Slice Bad() { std::string s = "hello"; return Slice(s); }  // !!! 错误
// 正确的做法是让调用方保证底层 buffer 的生命周期：
//     void Good(std::string* buf, Slice* out) { *out = Slice(*buf); }
// ===========================================================================
class Slice {
public:
    // 构造一个空 Slice。data_ 指向一个静态空串而非 nullptr，
    // 这样 data() 永远可以安全地传给 memcmp（对空区间 memcmp 是合法的）。
    Slice() noexcept : data_(""), size_(0) {}

    Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}

    // NOLINTNEXTLINE(google-explicit-constructor): 允许隐式转换，这是故意的设计
    Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}

    // NOLINTNEXTLINE(google-explicit-constructor): 允许字符串字面量隐式转换
    Slice(const char* s) noexcept : data_(s), size_(std::strlen(s)) {}

    // 默认拷贝 / 赋值 / 析构即可：Slice 只是值语义的 (ptr, len)，不需要所有权管理。
    Slice(const Slice&) noexcept = default;
    Slice& operator=(const Slice&) noexcept = default;
    ~Slice() = default;

    const char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // 不做边界检查（与 std::string_view::operator[] 一致）。
    // 设计取舍：热路径上每次都检查边界开销不可忽略，且调用方通常已从
    // size() 得到了足够信息。调试阶段由 ASAN 兜底。
    char operator[](size_t n) const noexcept { return data_[n]; }

    void clear() noexcept {
        data_ = "";
        size_ = 0;
    }

    // 常用于 varint / 长度前缀的解析：读完一个字段后把游标往前推。
    void remove_prefix(size_t n) noexcept {
        data_ += n;
        size_ -= n;
    }

    std::string ToString() const { return std::string(data_, size_); }

    // 三路比较：先用 memcmp 比公共前缀，再比长度。
    // 注意这与 std::string::compare 一致，是"字节序"而非"字典序"，
    // 对含 '\0' 的二进制 key 也能给出确定的全序关系。
    int compare(const Slice& b) const noexcept;

    bool starts_with(const Slice& x) const noexcept;

private:
    const char* data_;
    size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) noexcept {
    return x.size() == y.size() &&
           std::memcmp(x.data(), y.data(), x.size()) == 0;
}

inline bool operator!=(const Slice& x, const Slice& y) noexcept {
    return !(x == y);
}

inline int Slice::compare(const Slice& b) const noexcept {
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    const int r = std::memcmp(data_, b.data_, min_len);
    if (r != 0) return r;
    // 公共前缀相同，则短者更小
    if (size_ < b.size_) return -1;
    if (size_ > b.size_) return +1;
    return 0;
}

inline bool Slice::starts_with(const Slice& x) const noexcept {
    return size_ >= x.size_ && std::memcmp(data_, x.data_, x.size_) == 0;
}

// 供 gtest 在断言失败时打印 Slice 的内容
inline std::ostream& operator<<(std::ostream& os, const Slice& s) {
    return os << s.ToString();
}

}  // namespace tinystore
