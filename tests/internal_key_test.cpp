#include "tinystore/internal_key.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

// 便捷构造：把 (user_key, seq, type) 编码成 std::string
std::string MakeKey(const Slice& user_key, SequenceNumber seq,
                    ValueType type = kTypeValue) {
    std::string encoded;
    AppendInternalKey(&encoded, ParsedInternalKey(user_key, seq, type));
    return encoded;
}

// ---------------------------------------------------------------------------
// 编码 / 解析
// ---------------------------------------------------------------------------

TEST(InternalKeyTest, AppendAndParseRoundTrip) {
    const std::string encoded = MakeKey("hello", 12345);

    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(encoded), &parsed));
    EXPECT_EQ(Slice("hello"), parsed.user_key);
    EXPECT_EQ(12345u, parsed.sequence);
    EXPECT_EQ(kTypeValue, parsed.type);
}

TEST(InternalKeyTest, AppendAddsExactlyEightByteSuffix) {
    std::string encoded;
    AppendInternalKey(&encoded, ParsedInternalKey("abc", 1, kTypeDeletion));

    ASSERT_EQ(3u + kInternalKeySuffixSize, encoded.size());
    EXPECT_EQ(8u, kInternalKeySuffixSize);
}

TEST(InternalKeyTest, DeletionTypeIsPreserved) {
    const std::string encoded = MakeKey("key", 7, kTypeDeletion);

    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(encoded), &parsed));
    EXPECT_EQ(kTypeDeletion, parsed.type);
    EXPECT_NE(kTypeValue, parsed.type);
}

TEST(InternalKeyTest, EmptyUserKeyIsAllowed) {
    const std::string encoded = MakeKey(Slice(), 99);

    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(encoded), &parsed));
    EXPECT_EQ(0u, parsed.user_key.size());
    EXPECT_EQ(99u, parsed.sequence);
}

TEST(InternalKeyTest, BinaryUserKeyIsPreserved) {
    const std::string encoded = MakeKey(Slice("a\0b", 3), 5);

    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(encoded), &parsed));
    EXPECT_EQ(Slice("a\0b", 3), parsed.user_key);
}

TEST(InternalKeyTest, MaxSequenceNumberRoundTrips) {
    const std::string encoded = MakeKey("k", kMaxSequenceNumber);

    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(Slice(encoded), &parsed));
    EXPECT_EQ(kMaxSequenceNumber, parsed.sequence);
}

// ---------------------------------------------------------------------------
// 损坏数据的处理
// ---------------------------------------------------------------------------

TEST(InternalKeyTest, ParseRejectsTooShortInput) {
    ParsedInternalKey parsed;
    for (size_t len = 0; len < kInternalKeySuffixSize; ++len) {
        const std::string data(len, 'x');
        EXPECT_FALSE(ParseInternalKey(Slice(data), &parsed)) << "len=" << len;
    }
}

TEST(InternalKeyTest, ParseRejectsUnknownValueType) {
    // 手工构造一个 type 字段为 0x7（非法值）的内部键
    std::string encoded;
    encoded.append("key");
    PutFixed64(&encoded, PackSequenceAndType(1, kTypeValue));

    // 注意下标：packed 值按**小端序**写入，因此 type（packed 的最低字节）
    // 落在 encoded[3]，而不是最后一个字节。
    // 这正是本模块反复强调的"小端序陷阱"——写测试时同样会踩到。
    const size_t type_offset = encoded.size() - kInternalKeySuffixSize;
    encoded[type_offset] = static_cast<char>(0x7);

    ParsedInternalKey parsed;
    EXPECT_FALSE(ParseInternalKey(Slice(encoded), &parsed));
}

// ---------------------------------------------------------------------------
// 打包 / 解包
// ---------------------------------------------------------------------------

TEST(InternalKeyTest, PackSequenceAndTypeLayout) {
    // 高 56 位是 sequence，低 8 位是 type。
    // 注意 kTypeValue == 0x1、kTypeDeletion == 0x0，
    // 所以 seq=1 时：value 型是 0x0101，deletion 型是 0x0100。
    EXPECT_EQ(0x0101u, PackSequenceAndType(1, kTypeValue));
    EXPECT_EQ(0x0100u, PackSequenceAndType(1, kTypeDeletion));
    EXPECT_EQ(0u, PackSequenceAndType(0, kTypeDeletion));
    EXPECT_EQ(0x0001u, PackSequenceAndType(0, kTypeValue));
    EXPECT_EQ(kMaxSequenceNumber << 8 | kTypeValue,
              PackSequenceAndType(kMaxSequenceNumber, kTypeValue));
}

TEST(InternalKeyTest, ExtractUserKeyStripsSuffix) {
    const std::string encoded = MakeKey("user-key", 42);
    EXPECT_EQ(Slice("user-key"), ExtractUserKey(Slice(encoded)));
}

// ---------------------------------------------------------------------------
// 排序规则 —— 本文件最重要的部分
// ---------------------------------------------------------------------------

TEST(InternalKeyTest, DifferentUserKeysSortAscending) {
    const InternalKeyComparator icmp(BytewiseComparator());

    EXPECT_LT(icmp.Compare(MakeKey("a", 100), MakeKey("b", 1)), 0);
    EXPECT_GT(icmp.Compare(MakeKey("b", 1), MakeKey("a", 100)), 0);

    // user_key 的优先级高于 sequence：
    // 即使 "b" 的 sequence 小得多，它仍然排在 "a" 之后
}

TEST(InternalKeyTest, SameUserKeySortsBySequenceDescending) {
    const InternalKeyComparator icmp(BytewiseComparator());

    const std::string older = MakeKey("key", 100);
    const std::string newer = MakeKey("key", 200);

    // sequence 越大 = 越新 = 排序越靠前
    EXPECT_GT(icmp.Compare(older, newer), 0);
    EXPECT_LT(icmp.Compare(newer, older), 0);
    EXPECT_EQ(0, icmp.Compare(older, MakeKey("key", 100)));
}

// ---------------------------------------------------------------------------
// 这条性质是整个 MVCC 的基础：在有序结构里，
// 对某个 user_key 遇到的第一个匹配项就是最新版本。
// ---------------------------------------------------------------------------
TEST(InternalKeyTest, NewestVersionAppearsFirstInSortedOrder) {
    const InternalKeyComparator icmp(BytewiseComparator());

    auto less = [&icmp](const std::string& a, const std::string& b) {
        return icmp.Compare(a, b) < 0;
    };
    std::set<std::string, decltype(less)> ordered(less);

    // 乱序插入 5 个版本
    ordered.insert(MakeKey("a", 10));
    ordered.insert(MakeKey("a", 30));
    ordered.insert(MakeKey("a", 20));
    ordered.insert(MakeKey("b", 5));
    ordered.insert(MakeKey("b", 7));

    ASSERT_EQ(5u, ordered.size());

    std::vector<std::string> actual(ordered.begin(), ordered.end());
    const std::vector<std::string> expected = {
        MakeKey("a", 30),  // "a" 的最新版本
        MakeKey("a", 20),
        MakeKey("a", 10),
        MakeKey("b", 7),   // "b" 的最新版本
        MakeKey("b", 5),
    };

    EXPECT_EQ(expected, actual);
}

TEST(InternalKeyTest, DeletionTombstoneSortsBeforeOlderValues) {
    const InternalKeyComparator icmp(BytewiseComparator());

    // 删除操作的 sequence 总是最新，因此墓碑排在最前面，
    // 查找时第一个就遇到它 —— 这就是"删除"语义的实现方式
    const std::string value = MakeKey("k", 10, kTypeValue);
    const std::string tombstone = MakeKey("k", 20, kTypeDeletion);

    EXPECT_LT(icmp.Compare(tombstone, value), 0);
}

// ---------------------------------------------------------------------------
// 这个测试专门验证"后缀不能用 memcmp 比较"这一结论。
// 如果实现里错误地用了 memcmp，下面这个用例会失败。
// ---------------------------------------------------------------------------
TEST(InternalKeyTest, SuffixComparisonMustBeNumericNotMemcmp) {
    const InternalKeyComparator icmp(BytewiseComparator());

    // 构造两个后缀在小端 memcmp 下与数值序相反的情况：
    //   seq=256 -> packed = 0x0100 -> 小端字节 [00][01]...
    //   seq=255 -> packed = 0x00ff -> 小端字节 [ff][00]...
    // memcmp 会认为 256 < 255（因为第一个字节 0x00 < 0xff），
    // 但数值上 256 > 255，因此"更新"的应该是 seq=256 那个。
    const std::string seq_256 = MakeKey("k", 256);
    const std::string seq_255 = MakeKey("k", 255);

    EXPECT_LT(icmp.Compare(seq_256, seq_255), 0)
        << "seq=256 更新，必须排在 seq=255 前面；"
           "若此处失败，说明实现里误用了 memcmp 比较后缀";
}

// ---------------------------------------------------------------------------
// InternalKey 类
// ---------------------------------------------------------------------------

TEST(InternalKeyTest, InternalKeyClassOwnsItsStorage) {
    InternalKey key(Slice("owned"), 77, kTypeValue);

    EXPECT_TRUE(key.valid());
    EXPECT_EQ(Slice("owned"), key.user_key());

    const Slice encoded = key.Encode();
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(encoded, &parsed));
    EXPECT_EQ(77u, parsed.sequence);
    EXPECT_EQ(kTypeValue, parsed.type);
}

TEST(InternalKeyTest, InternalKeySetFromAndClear) {
    InternalKey key;
    EXPECT_FALSE(key.valid());

    key.SetFrom(ParsedInternalKey(Slice("k"), 5, kTypeValue));
    EXPECT_TRUE(key.valid());
    EXPECT_EQ(Slice("k"), key.user_key());

    key.Clear();
    EXPECT_FALSE(key.valid());
}

TEST(InternalKeyTest, InternalKeyDecodeFromEncodedSlice) {
    const std::string encoded = MakeKey("decoded", 3);

    InternalKey key;
    key.DecodeFrom(Slice(encoded));
    EXPECT_EQ(encoded, key.Encode().ToString());
}

TEST(InternalKeyTest, InternalKeyCompareOverload) {
    const InternalKeyComparator icmp(BytewiseComparator());

    const InternalKey older(Slice("k"), 1, kTypeValue);
    const InternalKey newer(Slice("k"), 2, kTypeValue);

    EXPECT_GT(icmp.Compare(older, newer), 0);
    EXPECT_LT(icmp.Compare(newer, older), 0);
}

TEST(InternalKeyTest, CompareUserKeyIgnoresSequence) {
    const InternalKeyComparator icmp(BytewiseComparator());

    const std::string a = MakeKey("k", 1);
    const std::string b = MakeKey("k", 999);

    // CompareUserKey 只比较 user_key 部分
    EXPECT_EQ(0, icmp.CompareUserKey(a, b));
    EXPECT_LT(icmp.CompareUserKey(MakeKey("a", 1), MakeKey("b", 1)), 0);
}

TEST(InternalKeyTest, DebugStringIsParseable) {
    const InternalKey key(Slice("dbg"), 12, kTypeDeletion);
    const std::string debug = key.DebugString();

    // DebugString 里应包含 user_key、sequence 和 type
    EXPECT_NE(std::string::npos, debug.find("dbg"));
    EXPECT_NE(std::string::npos, debug.find("12"));
    EXPECT_NE(std::string::npos, debug.find("0"));
}

}  // namespace
}  // namespace tinystore
