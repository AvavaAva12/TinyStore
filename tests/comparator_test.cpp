#include "tinystore/comparator.h"

#include <string>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

TEST(ComparatorTest, BytewiseCompareUsesByteOrder) {
    const Comparator* cmp = BytewiseComparator();

    EXPECT_EQ(0, cmp->Compare("abc", "abc"));
    EXPECT_LT(cmp->Compare("abc", "abd"), 0);
    EXPECT_GT(cmp->Compare("abd", "abc"), 0);

    // 短前缀更小
    EXPECT_LT(cmp->Compare("ab", "abc"), 0);
    EXPECT_GT(cmp->Compare("abc", "ab"), 0);

    EXPECT_LT(cmp->Compare(Slice(), "a"), 0);
}

TEST(ComparatorTest, BytewiseCompareIsBinarySafe) {
    const Comparator* cmp = BytewiseComparator();

    // 含 '\0' 的 key：strcmp 会在这里失效，memcmp 才正确
    EXPECT_LT(cmp->Compare(Slice("a\0b", 3), Slice("a\0c", 3)), 0);
    EXPECT_EQ(0, cmp->Compare(Slice("a\0b", 3), Slice("a\0b", 3)));
}

TEST(ComparatorTest, NameIsStableAndNonEmpty) {
    const Comparator* cmp = BytewiseComparator();
    ASSERT_NE(nullptr, cmp->Name());
    EXPECT_GT(std::string(cmp->Name()).size(), 0u);
    // 多次调用应返回同一个单例
    EXPECT_EQ(cmp, BytewiseComparator());
}

// ---------------------------------------------------------------------------
// FindShortestSeparator
// ---------------------------------------------------------------------------

TEST(ComparatorTest, FindShortestSeparatorShortensKey) {
    const Comparator* cmp = BytewiseComparator();

    std::string start = "abcd";
    cmp->FindShortestSeparator(&start, Slice("abcf"));
    // 'd'(0x64) + 1 = 'e'(0x65) < 'f'(0x66)，可以进位并截断
    EXPECT_EQ("abce", start);
    EXPECT_LT(cmp->Compare(start, "abcf"), 0);
}

TEST(ComparatorTest, FindShortestSeparatorKeepsKeyWhenNoRoomToIncrement) {
    const Comparator* cmp = BytewiseComparator();

    std::string start = "abcd";
    cmp->FindShortestSeparator(&start, Slice("abce"));
    // 'd' + 1 == 'e'，不小于 limit，无法生成中间键，保持原样
    EXPECT_EQ("abcd", start);
}

TEST(ComparatorTest, FindShortestSeparatorTruncatesLongKeys) {
    const Comparator* cmp = BytewiseComparator();

    std::string start = "aaaaaaaaaaaaaaaa";
    cmp->FindShortestSeparator(&start, Slice("b"));
    // 第一个字节 'a' -> 'b' 后仍应 < "b"？不：'b' 不小于 'b'，所以不进位。
    // 但前 16 字节里 start 全是 'a'，limit 只有 1 字节 'b'，
    // min_length = 1，diff_index = 0，'a'+1='b' 不小于 'b' -> 保持原样
    EXPECT_EQ("aaaaaaaaaaaaaaaa", start);

    std::string start2 = "aaaaaaaaaaaaaaaa";
    cmp->FindShortestSeparator(&start2, Slice("c"));
    // 'a'+1='b' < 'c' -> 截断成 "b"
    EXPECT_EQ("b", start2);
    EXPECT_LT(cmp->Compare(start2, "c"), 0);
}

TEST(ComparatorTest, FindShortestSeparatorWhenStartIsPrefixOfLimit) {
    const Comparator* cmp = BytewiseComparator();

    std::string start = "ab";
    cmp->FindShortestSeparator(&start, Slice("abcd"));
    // start 是 limit 的前缀，任何修改都无法保证 < limit
    EXPECT_EQ("ab", start);
}

TEST(ComparatorTest, FindShortestSeparatorWithMaxByte) {
    const Comparator* cmp = BytewiseComparator();

    std::string start(1, static_cast<char>(0xfe));
    cmp->FindShortestSeparator(&start, Slice(std::string(1, static_cast<char>(0xff))));
    // 0xfe + 1 = 0xff，不小于 limit 的 0xff -> 不进位
    EXPECT_EQ(std::string(1, static_cast<char>(0xfe)), start);
}

// ---------------------------------------------------------------------------
// FindShortSuccessor
// ---------------------------------------------------------------------------

TEST(ComparatorTest, FindShortSuccessorIncrementsFirstByte) {
    const Comparator* cmp = BytewiseComparator();

    std::string key = "abcd";
    cmp->FindShortSuccessor(&key);
    EXPECT_EQ("b", key);  // 'a' -> 'b'，截断到长度 1
    EXPECT_GT(cmp->Compare(key, "abcd"), 0);
}

TEST(ComparatorTest, FindShortSuccessorSkipsMaxBytes) {
    const Comparator* cmp = BytewiseComparator();

    // 首字节是 0xff 无法进位，跳到下一个字节
    std::string key = std::string(1, static_cast<char>(0xff)) + "a";
    cmp->FindShortSuccessor(&key);
    EXPECT_EQ(std::string(1, static_cast<char>(0xff)) + "b", key);
    EXPECT_GT(cmp->Compare(key, std::string(1, static_cast<char>(0xff)) + "a"), 0);
}

TEST(ComparatorTest, FindShortSuccessorAllMaxBytesIsUnchanged) {
    const Comparator* cmp = BytewiseComparator();

    const std::string original(3, static_cast<char>(0xff));
    std::string key = original;
    cmp->FindShortSuccessor(&key);
    EXPECT_EQ(original, key);
}

TEST(ComparatorTest, FindShortSuccessorOnEmptyKey) {
    const Comparator* cmp = BytewiseComparator();

    std::string key;
    cmp->FindShortSuccessor(&key);
    EXPECT_TRUE(key.empty());
}

// ---------------------------------------------------------------------------
// 这两个方法的不变式（invariant）值得用随机数据做一次性质测试，
// 比逐个列举用例更能覆盖边界。
// ---------------------------------------------------------------------------
TEST(ComparatorTest, SuccessorAndSeparatorPreserveOrderingInvariant) {
    const Comparator* cmp = BytewiseComparator();

    for (int i = 0; i < 200; ++i) {
        const std::string base = "key" + std::to_string(i);

        std::string successor = base;
        cmp->FindShortSuccessor(&successor);
        EXPECT_GT(cmp->Compare(successor, base), 0)
            << "successor of " << base << " must be strictly greater";
        EXPECT_LE(successor.size(), base.size())
            << "successor must not grow";

        // 取一个更大的 limit，验证 separator 落在 (base, limit) 之间
        const std::string limit = base + "zzz";
        std::string separator = base;
        cmp->FindShortestSeparator(&separator, Slice(limit));
        EXPECT_GE(cmp->Compare(separator, base), 0);
        EXPECT_LT(cmp->Compare(separator, limit), 0);
    }
}

}  // namespace
}  // namespace tinystore
