#include "tinystore/coding.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

// ---------------------------------------------------------------------------
// 定长编码
// ---------------------------------------------------------------------------

TEST(CodingTest, Fixed32ByteLayoutIsLittleEndian) {
    char buf[4];
    EncodeFixed32(buf, 0x01020304u);

    // 小端序：最低有效字节在最低地址
    EXPECT_EQ(0x04, static_cast<unsigned char>(buf[0]));
    EXPECT_EQ(0x03, static_cast<unsigned char>(buf[1]));
    EXPECT_EQ(0x02, static_cast<unsigned char>(buf[2]));
    EXPECT_EQ(0x01, static_cast<unsigned char>(buf[3]));
}

TEST(CodingTest, Fixed32RoundTrip) {
    const std::vector<uint32_t> cases = {0u,
                                         1u,
                                         0xffu,
                                         0x01020304u,
                                         0x80000000u,
                                         0xffffffffu};

    for (uint32_t value : cases) {
        std::string s;
        PutFixed32(&s, value);
        ASSERT_EQ(4u, s.size());
        EXPECT_EQ(value, DecodeFixed32(s.data()));
    }
}

TEST(CodingTest, Fixed64RoundTrip) {
    const std::vector<uint64_t> cases = {0ull,
                                         1ull,
                                         0x0102030405060708ull,
                                         0x8000000000000000ull,
                                         0xffffffffffffffffull};

    for (uint64_t value : cases) {
        std::string s;
        PutFixed64(&s, value);
        ASSERT_EQ(8u, s.size());
        EXPECT_EQ(value, DecodeFixed64(s.data()));
    }
}

// ---------------------------------------------------------------------------
// 这个测试专门针对"char 有符号导致符号扩展"的经典 bug。
// 若 DecodeFixed32 里忘了先转 unsigned char，
// 0x80 会被当成 -128 并符号扩展成 0xffffff80，结果错误。
// ---------------------------------------------------------------------------
TEST(CodingTest, Fixed32HighBitDoesNotSignExtend) {
    std::string s;
    PutFixed32(&s, 0x80u);
    EXPECT_EQ(0x80u, DecodeFixed32(s.data()));

    s.clear();
    PutFixed32(&s, 0xffffff80u);
    EXPECT_EQ(0xffffff80u, DecodeFixed32(s.data()));

    s.clear();
    PutFixed64(&s, 0xff00000000000000ull);
    EXPECT_EQ(0xff00000000000000ull, DecodeFixed64(s.data()));
}

// ---------------------------------------------------------------------------
// Varint
// ---------------------------------------------------------------------------

TEST(CodingTest, Varint32RoundTrip) {
    const std::vector<uint32_t> cases = {0u,        1u,
                                         127u,      128u,       // 1 -> 2 字节边界
                                         129u,      300u,
                                         16383u,    16384u,     // 2 -> 3 字节边界
                                         2097151u,  2097152u,   // 3 -> 4 字节边界
                                         268435455u,
                                         268435456u,            // 4 -> 5 字节边界
                                         0xffffffffu};

    for (uint32_t value : cases) {
        std::string s;
        PutVarint32(&s, value);
        EXPECT_EQ(VarintLength(value), static_cast<int>(s.size()))
            << "value=" << value;

        Slice input(s);
        uint32_t parsed = 0;
        ASSERT_TRUE(GetVarint32(&input, &parsed)) << "value=" << value;
        EXPECT_EQ(value, parsed);
        EXPECT_TRUE(input.empty());  // 应恰好消耗完
    }
}

TEST(CodingTest, Varint64RoundTrip) {
    const std::vector<uint64_t> cases = {0ull,
                                         1ull,
                                         127ull,
                                         128ull,
                                         16384ull,
                                         (1ull << 40),
                                         (1ull << 56),
                                         0xffffffffffffffffull};

    for (uint64_t value : cases) {
        std::string s;
        PutVarint64(&s, value);

        Slice input(s);
        uint64_t parsed = 0;
        ASSERT_TRUE(GetVarint64(&input, &parsed)) << "value=" << value;
        EXPECT_EQ(value, parsed);
    }
}

TEST(CodingTest, VarintLengthBoundaries) {
    EXPECT_EQ(1, VarintLength(0));
    EXPECT_EQ(1, VarintLength(127));
    EXPECT_EQ(2, VarintLength(128));
    EXPECT_EQ(2, VarintLength(16383));
    EXPECT_EQ(3, VarintLength(16384));
    EXPECT_EQ(5, VarintLength(0xffffffffu));
    EXPECT_EQ(10, VarintLength(0xffffffffffffffffull));
}

// ---------------------------------------------------------------------------
// 截断与损坏数据的处理：必须返回 false，而不是崩溃或返回错误的值。
// 磁盘上的数据损坏是存储引擎的常态，这条路径必须有确定行为。
// ---------------------------------------------------------------------------
TEST(CodingTest, TruncatedVarintReturnsFalse) {
    // 编码一个需要 3 字节的值，然后逐个截断
    std::string s;
    PutVarint32(&s, 16384u);
    ASSERT_EQ(3u, s.size());

    for (size_t len = 0; len < s.size(); ++len) {
        Slice input(s.data(), len);
        uint32_t parsed = 0;
        EXPECT_FALSE(GetVarint32(&input, &parsed)) << "len=" << len;
    }
}

TEST(CodingTest, VarintWithAllContinuationBitsSetIsRejected) {
    // 5 个字节全部带续位标志，第 5 个之后仍是 0x80 —— 非法
    const std::string s(5, static_cast<char>(0x80));
    Slice input(s);
    uint32_t parsed = 0;
    EXPECT_FALSE(GetVarint32(&input, &parsed));
}

TEST(CodingTest, VarintAdvancesInputCorrectly) {
    std::string s;
    PutVarint32(&s, 1u);
    PutVarint32(&s, 300u);
    s.append("tail");

    Slice input(s);
    uint32_t first = 0;
    ASSERT_TRUE(GetVarint32(&input, &first));
    EXPECT_EQ(1u, first);

    uint32_t second = 0;
    ASSERT_TRUE(GetVarint32(&input, &second));
    EXPECT_EQ(300u, second);

    EXPECT_EQ(Slice("tail"), input);  // 剩余部分应正好是 tail
}

// ---------------------------------------------------------------------------
// 长度前缀 Slice
// ---------------------------------------------------------------------------

TEST(CodingTest, LengthPrefixedSliceRoundTrip) {
    std::string s;
    PutLengthPrefixedSlice(&s, Slice("hello"));
    PutLengthPrefixedSlice(&s, Slice(""));
    PutLengthPrefixedSlice(&s, Slice("a\0b", 3));  // 二进制安全

    Slice input(s);
    Slice result;

    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &result));
    EXPECT_EQ(Slice("hello"), result);

    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &result));
    EXPECT_EQ(Slice(""), result);
    EXPECT_EQ(0u, result.size());

    ASSERT_TRUE(GetLengthPrefixedSlice(&input, &result));
    EXPECT_EQ(Slice("a\0b", 3), result);

    EXPECT_TRUE(input.empty());
}

TEST(CodingTest, LengthPrefixedSliceDetectsTruncation) {
    std::string s;
    PutLengthPrefixedSlice(&s, Slice("hello world"));

    // 声明的长度是 11，但实际只给了 5 个字节 payload
    const std::string truncated = s.substr(0, 1 + 5);
    Slice input(truncated);
    Slice result;
    EXPECT_FALSE(GetLengthPrefixedSlice(&input, &result));
}

}  // namespace
}  // namespace tinystore
