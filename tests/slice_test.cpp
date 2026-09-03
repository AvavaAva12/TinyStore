#include "tinystore/slice.h"

#include <string>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

TEST(SliceTest, DefaultConstructedIsEmpty) {
    const Slice s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(0u, s.size());
    EXPECT_EQ(Slice(""), s);
    // data() 永远不应返回 nullptr，这样 memcmp(empty, empty) 才合法
    ASSERT_NE(nullptr, s.data());
}

TEST(SliceTest, ConstructFromCString) {
    const Slice s("hello");
    EXPECT_EQ(5u, s.size());
    EXPECT_EQ(std::string("hello"), s.ToString());
    EXPECT_EQ('h', s[0]);
    EXPECT_EQ('o', s[4]);
}

TEST(SliceTest, ConstructFromStdString) {
    const std::string backing = "world";
    const Slice s(backing);
    // Slice 不拷贝数据，data() 应直接指向 backing 的内部缓冲
    EXPECT_EQ(backing.data(), s.data());
    EXPECT_EQ(5u, s.size());
}

TEST(SliceTest, ConstructWithExplicitLength) {
    const char buffer[] = "hello world";
    const Slice s(buffer, 5);
    EXPECT_EQ(std::string("hello"), s.ToString());
}

TEST(SliceTest, CompareEqualAndDifferentLengths) {
    EXPECT_EQ(0, Slice("abc").compare(Slice("abc")));
    EXPECT_LT(Slice("abc").compare(Slice("abd")), 0);
    EXPECT_GT(Slice("abd").compare(Slice("abc")), 0);

    // 前缀更短者更小 —— 这是存储引擎做范围查找的基础性质
    EXPECT_LT(Slice("ab").compare(Slice("abc")), 0);
    EXPECT_GT(Slice("abc").compare(Slice("ab")), 0);
}

TEST(SliceTest, ComparisonOperators) {
    EXPECT_TRUE(Slice("abc") == Slice("abc"));
    EXPECT_FALSE(Slice("abc") == Slice("abd"));
    EXPECT_TRUE(Slice("abc") != Slice("abd"));

    // 只有长度和内容都相同时才相等
    EXPECT_FALSE(Slice("ab") == Slice("abc"));
}

TEST(SliceTest, StartsWith) {
    EXPECT_TRUE(Slice("hello world").starts_with(Slice("hello")));
    EXPECT_TRUE(Slice("hello").starts_with(Slice("hello")));
    EXPECT_TRUE(Slice("hello").starts_with(Slice()));  // 空串是任何串的前缀
    EXPECT_FALSE(Slice("hello").starts_with(Slice("world")));
    EXPECT_FALSE(Slice("ab").starts_with(Slice("abc")));  // 比自己长
}

TEST(SliceTest, RemovePrefix) {
    Slice s("hello world");
    s.remove_prefix(6);
    EXPECT_EQ(std::string("world"), s.ToString());

    s.remove_prefix(5);
    EXPECT_TRUE(s.empty());
}

TEST(SliceTest, Clear) {
    Slice s("hello");
    ASSERT_FALSE(s.empty());
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(Slice(""), s);
}

// ---------------------------------------------------------------------------
// 二进制安全：key 里含 '\0' 是合法且常见的（例如 Protobuf 序列化后的 key）。
// 这是不能用 strcmp 而必须用 memcmp 的根本原因。
// ---------------------------------------------------------------------------
TEST(SliceTest, BinarySafeWithEmbeddedNulls) {
    const std::string a("a\0b", 3);
    const std::string b("a\0c", 3);

    EXPECT_EQ(3u, Slice(a).size());  // 没有被 '\0' 截断
    EXPECT_EQ(0, Slice(a).compare(Slice(a)));
    EXPECT_LT(Slice(a).compare(Slice(b)), 0);
    EXPECT_NE(Slice(a), Slice(b));
}

// ---------------------------------------------------------------------------
// Slice 是值语义的 (ptr, len) 对，拷贝后两者指向同一块内存。
// 这个测试把"非拥有"的语义显式固定下来，防止未来误改成深拷贝。
// ---------------------------------------------------------------------------
TEST(SliceTest, CopySharesUnderlyingMemory) {
    const std::string backing = "shared data";
    const Slice original(backing);
    const Slice copy = original;

    EXPECT_EQ(original.data(), copy.data());
    EXPECT_EQ(original.size(), copy.size());
}

TEST(SliceTest, EmptySlicesAreAlwaysEqual) {
    // 两个空 Slice 的 data() 可能指向不同的内存，但仍应判等
    EXPECT_EQ(Slice(), Slice(""));
    EXPECT_EQ(Slice(""), Slice(std::string()));
}

}  // namespace
}  // namespace tinystore
