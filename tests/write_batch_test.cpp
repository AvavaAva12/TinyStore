#include "tinystore/write_batch.h"
#include "tinystore/memtable.h"
#include "tinystore/comparator.h"
#include "tinystore/internal_key.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tinystore {

namespace {
InternalKeyComparator ikc(BytewiseComparator());

class Collector : public WriteBatch::Handler {
public:
  std::vector<std::string> ops;
  void Put(const Slice& k, const Slice& v) override {
    ops.push_back("P:" + k.ToString() + "=" + v.ToString());
  }
  void Delete(const Slice& k) override { ops.push_back("D:" + k.ToString()); }
};
}  // namespace

TEST(WriteBatchTest, PutDeleteCount) {
  WriteBatch b;
  EXPECT_EQ(b.Count(), 0u);
  b.Put(Slice("a"), Slice("1"));
  b.Put(Slice("b"), Slice("2"));
  b.Delete(Slice("c"));
  EXPECT_EQ(b.Count(), 3u);
}

TEST(WriteBatchTest, IterateReproduces) {
  WriteBatch b;
  b.Put(Slice("a"), Slice("1"));
  b.Delete(Slice("b"));
  Collector c;
  ASSERT_TRUE(b.Iterate(&c).ok());
  EXPECT_EQ(c.ops, (std::vector<std::string>{"P:a=1", "D:b"}));
}

TEST(WriteBatchTest, InsertIntoMemTable) {
  WriteBatch b;
  b.Put(Slice("a"), Slice("1"));
  b.Put(Slice("b"), Slice("2"));
  b.Delete(Slice("c"));
  WriteBatchInternal::SetSequence(&b, 100);

  MemTable mt(&ikc);
  WriteBatchInternal::InsertInto(&b, &mt);

  std::string v;
  ASSERT_TRUE(mt.Get(Slice("a"), 100, &v).ok());
  EXPECT_EQ(v, "1");
  ASSERT_TRUE(mt.Get(Slice("b"), 101, &v).ok());
  EXPECT_EQ(v, "2");
  EXPECT_TRUE(mt.Get(Slice("c"), 102, &v).IsNotFound());

  // 回放后 batch 的起始 sequence 应推进到最后一条之后
  EXPECT_EQ(WriteBatchInternal::Sequence(&b), 103u);
}

TEST(WriteBatchTest, AppendMerges) {
  WriteBatch a, b;
  a.Put(Slice("x"), Slice("1"));
  b.Put(Slice("y"), Slice("2"));
  b.Delete(Slice("z"));
  WriteBatchInternal::Append(&a, &b);

  EXPECT_EQ(a.Count(), 3u);
  Collector c;
  ASSERT_TRUE(a.Iterate(&c).ok());
  EXPECT_EQ(c.ops, (std::vector<std::string>{"P:x=1", "P:y=2", "D:z"}));
}

}  // namespace tinystore
