#include "tinystore/memtable.h"
#include "tinystore/comparator.h"
#include "tinystore/internal_key.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tinystore {

namespace {
InternalKeyComparator ikc(BytewiseComparator());
}  // namespace

TEST(MemTableTest, PutAndGet) {
  MemTable mt(&ikc);
  mt.Add(1, kTypeValue, Slice("foo"), Slice("bar"));

  std::string v;
  ASSERT_TRUE(mt.Get(Slice("foo"), 1, &v).ok());
  EXPECT_EQ(v, "bar");
  ASSERT_TRUE(mt.Get(Slice("foo"), kMaxSequenceNumber, &v).ok());
  EXPECT_EQ(v, "bar");

  EXPECT_TRUE(mt.Get(Slice("missing"), 1, &v).IsNotFound());
}

TEST(MemTableTest, SnapshotRead) {
  MemTable mt(&ikc);
  mt.Add(1, kTypeValue, Slice("k"), Slice("v1"));
  mt.Add(2, kTypeValue, Slice("k"), Slice("v2"));

  std::string v;
  // snapshot=1 应看到 v1（最新的 <=1 的版本）
  ASSERT_TRUE(mt.Get(Slice("k"), 1, &v).ok());
  EXPECT_EQ(v, "v1");
  // snapshot=2 应看到 v2
  ASSERT_TRUE(mt.Get(Slice("k"), 2, &v).ok());
  EXPECT_EQ(v, "v2");
  // snapshot=3 仍看到 v2
  ASSERT_TRUE(mt.Get(Slice("k"), 3, &v).ok());
  EXPECT_EQ(v, "v2");
}

TEST(MemTableTest, DeletionTombstone) {
  MemTable mt(&ikc);
  mt.Add(1, kTypeValue, Slice("k"), Slice("v1"));
  mt.Add(2, kTypeDeletion, Slice("k"), Slice());

  std::string v;
  ASSERT_TRUE(mt.Get(Slice("k"), 1, &v).ok());    // 删除之前可见
  EXPECT_TRUE(mt.Get(Slice("k"), 2, &v).IsNotFound());
  EXPECT_TRUE(mt.Get(Slice("k"), 3, &v).IsNotFound());

  // 删除之后再次写入，新版本应重新可见
  mt.Add(3, kTypeValue, Slice("k"), Slice("v3"));
  ASSERT_TRUE(mt.Get(Slice("k"), 3, &v).ok());
  EXPECT_EQ(v, "v3");
}

TEST(MemTableTest, IteratorInUserKeyOrder) {
  MemTable mt(&ikc);
  mt.Add(1, kTypeValue, Slice("a"), Slice("1"));
  mt.Add(2, kTypeValue, Slice("b"), Slice("2"));
  mt.Add(3, kTypeValue, Slice("c"), Slice("3"));

  MemTable::Iterator it(&mt);
  it.SeekToFirst();
  std::vector<std::string> keys;
  while (it.Valid()) {
    keys.push_back(it.user_key().ToString());
    it.Next();
  }
  EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));

  // Seek 到某 user_key，应得到其最新版本（seq 最大）
  it.Seek(Slice("b"));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.user_key().ToString(), "b");
  EXPECT_EQ(it.sequence(), 2u);
  EXPECT_EQ(it.value().ToString(), "2");
}

TEST(MemTableTest, MemoryUsagePositive) {
  MemTable mt(&ikc);
  mt.Add(1, kTypeValue, Slice("k"), Slice("value"));
  EXPECT_GT(mt.ApproximateMemoryUsage(), 0u);
}

}  // namespace tinystore
