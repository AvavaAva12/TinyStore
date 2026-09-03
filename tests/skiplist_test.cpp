#include "tinystore/skiplist.h"
#include "tinystore/arena.h"

#include <algorithm>
#include <random>
#include <vector>

#include "gtest/gtest.h"

namespace tinystore {

namespace {
struct IntComparator {
  int operator()(int a, int b) const {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
};
}  // namespace

TEST(SkipListTest, InsertAndContains) {
  Arena arena;
  SkipList<int, IntComparator> list(IntComparator{}, &arena);
  for (int i = 0; i < 1000; ++i) list.Insert(i);

  for (int i = 0; i < 1000; ++i) EXPECT_TRUE(list.Contains(i));
  EXPECT_FALSE(list.Contains(-1));
  EXPECT_FALSE(list.Contains(1000));
  EXPECT_FALSE(list.Contains(100000));
}

TEST(SkipListTest, IteratorVisitsInOrder) {
  Arena arena;
  SkipList<int, IntComparator> list(IntComparator{}, &arena);

  std::vector<int> v;
  for (int i = 0; i < 500; ++i) v.push_back(i);
  // 确定性打乱，避免依赖全局随机源（也便于复现失败）
  std::mt19937 rng(42);
  std::shuffle(v.begin(), v.end(), rng);
  for (int x : v) list.Insert(x);

  SkipList<int, IntComparator>::Iterator it(&list);
  it.SeekToFirst();
  int prev = -1;
  int count = 0;
  while (it.Valid()) {
    EXPECT_GT(it.key(), prev);  // 严格递增
    prev = it.key();
    it.Next();
    ++count;
  }
  EXPECT_EQ(count, 500);

  // Seek 落到 >= 250 的位置
  it.Seek(250);
  ASSERT_TRUE(it.Valid());
  EXPECT_GE(it.key(), 250);

  // 末尾 + 前驱
  it.SeekToLast();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 499);
  it.Prev();
  EXPECT_EQ(it.key(), 498);

  // Seek 一个不存在且过大的键 -> 无效
  it.Seek(99999);
  EXPECT_FALSE(it.Valid());
}

}  // namespace tinystore
