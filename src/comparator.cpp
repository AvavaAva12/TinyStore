#include "tinystore/comparator.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace tinystore {

namespace {

// ===========================================================================
// 字节序比较器：按 memcmp 的语义比较两个 key
// ===========================================================================
class BytewiseComparatorImpl final : public Comparator {
public:
    BytewiseComparatorImpl() = default;

    const char* Name() const override { return "tinystore.BytewiseComparator"; }

    int Compare(const Slice& a, const Slice& b) const override {
        return a.compare(b);
    }

    void FindShortestSeparator(std::string* start,
                               const Slice& limit) const override {
        // 找到第一个不同的字节
        const size_t min_length = std::min(start->size(), limit.size());
        size_t diff_index = 0;
        while (diff_index < min_length &&
               (*start)[diff_index] == limit[diff_index]) {
            ++diff_index;
        }

        if (diff_index >= min_length) {
            // *start 是 limit 的前缀（或两者相等）。
            // 此时任何对 *start 的修改都无法保证 < limit，只能原样返回。
            return;
        }

        const auto diff_byte = static_cast<uint8_t>((*start)[diff_index]);
        const auto limit_byte = static_cast<uint8_t>(limit[diff_index]);

        // 只有当 start 的这一位还能 +1 且 +1 之后仍严格小于 limit 时，
        // 才能安全地把它进位并截断到 diff_index + 1 长度。
        //
        // 例：start = "abcd", limit = "abcf"
        //   diff_index = 3, diff_byte = 'd'(0x64), limit_byte = 'f'(0x66)
        //   0x64 + 1 = 0x65 < 0x66  ->  结果是 "abce"，长度从 4 变成 4（此处不变）
        //
        // 例：start = "abcd", limit = "abce"
        //   0x64 + 1 = 0x65，不小于 0x65  ->  无解，保持原样
        if (diff_byte < 0xff && diff_byte + 1 < limit_byte) {
            (*start)[diff_index] = static_cast<char>(diff_byte + 1);
            start->resize(diff_index + 1);
            assert(Compare(*start, limit) < 0);
        }
    }

    void FindShortSuccessor(std::string* key) const override {
        // 必须**从前往后**找第一个可以 +1 的字节，而不是从后往前。
        //
        // 原因：字符串比较是从高位（下标小）开始的。只要把某个位置 i 的字节 +1，
        // 无论后面是什么，结果都必然大于原 key，因此可以直接把 i 之后全部截断。
        // 而要得到"最短"的后继，就应该选最小的可行 i，也就是从头扫描。
        //
        // 对比一下两种写法（key = "ab\xff"）：
        //   从前往后：i=0 处 'a' -> 'b'，截断成 "b"        （长度 1，最优）
        //   从后往前：i=2 处 'b' -> 'c'，截断成 "ac"       （长度 2，非最优）
        // 两者都满足 > "ab\xff"，但前者更短，索引块更小。
        for (size_t i = 0; i < key->size(); ++i) {
            const auto byte = static_cast<uint8_t>((*key)[i]);
            if (byte != 0xff) {
                (*key)[i] = static_cast<char>(byte + 1);
                key->resize(i + 1);
                return;
            }
        }
        // 整个 key 全是 0xff，不存在更短的严格后继，保持原样。
    }
};

}  // namespace

const Comparator* BytewiseComparator() {
    // 函数内静态变量：C++11 起初始化是线程安全的（magic statics），
    // 且首次调用才构造，避免了静态初始化顺序问题（static initialization order fiasco）。
    static const BytewiseComparatorImpl instance;
    return &instance;
}

}  // namespace tinystore
