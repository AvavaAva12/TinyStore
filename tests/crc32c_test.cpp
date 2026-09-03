#include "tinystore/crc32c.h"

#include "tinystore/slice.h"
#include "gtest/gtest.h"

namespace tinystore {

// "123456789" 是 CRC32C 的标准校验向量，结果应为 0xE3069283。
// 用它来确认查表算法与字节序处理都正确。
TEST(Crc32cTest, StandardCheckValue) {
  EXPECT_EQ(crc32c::Value(Slice("123456789")), 0xE3069283u);
}

TEST(Crc32cTest, EmptyIsZero) {
  EXPECT_EQ(crc32c::Value(Slice("")), 0u);
}

TEST(Crc32cTest, ExtendEquivalence) {
  const std::string full = "the quick brown fox";
  EXPECT_EQ(crc32c::Extend(0, full.data(), full.size()),
            crc32c::Value(Slice(full)));

  // 分段 extend 必须等于整段计算
  const std::string head = "the quick ";
  const std::string tail = "brown fox";
  const uint32_t c = crc32c::Extend(0, head.data(), head.size());
  EXPECT_EQ(crc32c::Extend(c, tail.data(), tail.size()),
            crc32c::Value(Slice(full)));
}

TEST(Crc32cTest, DifferentInputsDiffer) {
  EXPECT_NE(crc32c::Value(Slice("abc")), crc32c::Value(Slice("abd")));
}

}  // namespace tinystore
