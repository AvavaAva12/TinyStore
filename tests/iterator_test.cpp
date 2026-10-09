#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "tinystore/db_iterator.h"
#include "tinystore/env.h"
#include "tinystore/filter_policy.h"
#include "tinystore/internal_key.h"
#include "tinystore/table.h"
#include "gtest/gtest.h"

namespace tinystore {
namespace {

static std::string IK(const std::string& user_key, SequenceNumber seq,
                      ValueType type) {
  std::string k;
  AppendInternalKey(&k, ParsedInternalKey(user_key, seq, type));
  return k;
}

using Entry = std::pair<std::string, std::string>;
using Item = std::tuple<std::string, SequenceNumber, ValueType, std::string>;
using KV = std::pair<std::string, std::string>;

static std::vector<Entry> BuildEntries(const std::vector<Item>& items) {
  InternalKeyComparator icmp(BytewiseComparator());
  std::vector<Entry> out;
  out.reserve(items.size());
  for (const auto& it : items) {
    out.emplace_back(IK(std::get<0>(it), std::get<1>(it), std::get<2>(it)),
                     std::get<3>(it));
  }
  std::sort(out.begin(), out.end(), [&icmp](const Entry& a, const Entry& b) {
    return icmp.Compare(a.first, b.first) < 0;
  });
  return out;
}

static std::unique_ptr<Table> MakeTable(const InternalKeyComparator& icmp,
                                        const std::string& filename,
                                        const std::vector<Entry>& entries,
                                        size_t block_size = 128) {
  std::string path;
  Env::Default()->GetTestDirectory(&path);
  path += "/" + filename;
  {
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(Env::Default()->NewWritableFile(path, &file).ok());
    TableBuilder builder(&icmp, file.get(), DefaultFilterPolicy(), block_size);
    for (const auto& e : entries) builder.Add(e.first, e.second);
    EXPECT_TRUE(builder.Finish().ok());
    EXPECT_TRUE(file->Sync().ok());
    EXPECT_TRUE(file->Close().ok());
  }
  std::unique_ptr<RandomAccessFile> file;
  EXPECT_TRUE(Env::Default()->NewRandomAccessFile(path, &file).ok());
  uint64_t size = 0;
  EXPECT_TRUE(Env::Default()->GetFileSize(path, &size).ok());
  Table* table = nullptr;
  EXPECT_TRUE(Table::Open(&icmp, std::move(file), size, &table).ok());
  return std::unique_ptr<Table>(table);
}

static std::vector<KV> Collect(Iterator* it) {
  std::vector<KV> out;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(it->key().ToString(), it->value().ToString());
  }
  return out;
}

// Table 层迭代器按契约吐出完整 internal_key；断言顺序/内容时把 user_key 取出来，
// 免得每处都写一串带 \0 后缀的期望值。
static std::string UserKeyOf(const Iterator* it) {
  return ExtractUserKey(it->key()).ToString();
}

// ---------------------------------------------------------------------------
// Table 层：SSTable 自身的有序遍历
// ---------------------------------------------------------------------------

TEST(IteratorTest, TableScanIsOrderedAndComplete) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_scan.ldb",
                         BuildEntries({
                             {"apple", 1, kTypeValue, "A"},
                             {"banana", 1, kTypeValue, "B"},
                             {"cherry", 1, kTypeValue, "C"},
                             {"date", 1, kTypeValue, "D"},
                             {"elder", 1, kTypeValue, "E"},
                         }));

  auto it = table->NewIterator();
  std::vector<std::string> got;
  std::vector<std::string> values;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    got.push_back(UserKeyOf(it.get()));
    values.push_back(it->value().ToString());
  }
  std::vector<std::string> want = {"apple", "banana", "cherry", "date", "elder"};
  std::vector<std::string> want_vals = {"A", "B", "C", "D", "E"};
  EXPECT_EQ(got, want);
  EXPECT_EQ(values, want_vals);
}

// Table 层返回**完整 internal_key**、不做可见性过滤——这是 DBIterator 归并的
// 前提契约。若被改成只吐可见版本，上层会静默算错版本。
TEST(IteratorTest, TableIteratorYieldsRawInternalKeys) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_ikey.ldb",
                         BuildEntries({
                             {"a", 5, kTypeValue, "v5"},
                             {"a", 9, kTypeValue, "v9"},
                         }));

  std::vector<SequenceNumber> seqs;
  auto it = table->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ParsedInternalKey p;
    EXPECT_TRUE(ParseInternalKey(it->key(), &p));
    EXPECT_EQ("a", p.user_key.ToString());
    seqs.push_back(p.sequence);
  }
  // 两个版本都吐出来，且按 seq 降序（最新在前）
  ASSERT_EQ(seqs.size(), 2u);
  EXPECT_EQ(9u, seqs[0]);
  EXPECT_EQ(5u, seqs[1]);
}

TEST(IteratorTest, EmptyTableIteratorIsSafe) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_empty.ldb", {});

  auto it = table->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());

  it->Seek("zzz");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
}

// Seek 定位后连续 Next，应覆盖 Seek 位置到文件末尾的全部条目。
TEST(IteratorTest, TableIteratorSeekAndWalk) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_walk.ldb",
                         BuildEntries({
                             {"a", 1, kTypeValue, "1"},
                             {"c", 1, kTypeValue, "3"},
                             {"e", 1, kTypeValue, "5"},
                             {"g", 1, kTypeValue, "7"},
                         }));

  auto it = table->NewIterator();
  std::vector<std::string> seen;
  for (it->Seek("c"); it->Valid(); it->Next()) seen.push_back(UserKeyOf(it.get()));
  std::vector<std::string> want = {"c", "e", "g"};
  EXPECT_EQ(seen, want);
}

// ---------------------------------------------------------------------------
// DBIterator 层：MVCC 可见性过滤 + 跨源归并
// ---------------------------------------------------------------------------

// 同一 user_key 的多个版本，只输出快照下最新的可见版本。
TEST(IteratorTest, MergingIteratorOutputsLatestVisibleVersion) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_multi.ldb",
                         BuildEntries({
                             {"k", 5, kTypeValue, "A"},
                             {"k", 9, kTypeValue, "B"},
                             {"k", 20, kTypeValue, "C"},
                         }));

  struct Case {
    SequenceNumber snapshot;
    std::vector<KV> want;
  };
  // 边界值要覆盖：恰好可见(snapshot == seq)、刚好不可见(snapshot == seq - 1)。
  const std::vector<Case> cases = {
      {100, {{"k", "C"}}},  // 全部可见 -> 最新 seq 20
      {20, {{"k", "C"}}},  // seq 20 恰好可见
      {19, {{"k", "B"}}},  // seq 20 刚好不可见 -> seq 9
      {9, {{"k", "B"}}},   // seq 9 恰好可见
      {5, {{"k", "A"}}},   // 只有 seq 5 可见
      {4, {}},             // 全部版本都比快照新 -> key 不可见
  };
  for (const Case& c : cases) {
    DBIterator it(&icmp, c.snapshot);
    it.AddChild(table->NewIterator());
    EXPECT_EQ(Collect(&it), c.want) << "snapshot=" << c.snapshot;
  }
}

// 删除墓碑：快照看到墓碑时 key 整个不可见，且**不能回退到更老的值**
// （否则等于"删了又复活"）。
TEST(IteratorTest, MergingIteratorSkipsDeletedKeys) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_del.ldb",
                         BuildEntries({
                             {"gone", 3, kTypeValue, "old"},
                             {"gone", 7, kTypeDeletion, ""},
                             {"kept", 1, kTypeValue, "yes"},
                         }));

  struct Case {
    SequenceNumber snapshot;
    std::vector<KV> want;
  };
  const std::vector<Case> cases = {
      // seq7 的删除可见 -> gone 整个消失，绝不能输出 "old"
      {100, {{"kept", "yes"}}},
      {7, {{"kept", "yes"}}},
      // 删除还不可见（seq7 > snapshot）-> 露出 seq3 的旧值
      {6, {{"gone", "old"}, {"kept", "yes"}}},
      {3, {{"gone", "old"}, {"kept", "yes"}}},
      // 更早的快照：连 gone 的值都还没写入
      {2, {{"kept", "yes"}}},
  };
  for (const Case& c : cases) {
    DBIterator it(&icmp, c.snapshot);
    it.AddChild(table->NewIterator());
    EXPECT_EQ(Collect(&it), c.want) << "snapshot=" << c.snapshot;
  }
}

// 跨源归并：key 范围重叠的多个 SSTable 合并成一条有序流，不重不漏。
// （真实场景中每次 flush 的边界随意，L0 文件互相重叠，不能按文件顺序拼接。）
TEST(IteratorTest, MergingIteratorMergesOverlappingSources) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto t1 = MakeTable(icmp, "iter_m1.ldb",
                      BuildEntries({
                          {"a", 1, kTypeValue, "a1"},
                          {"c", 1, kTypeValue, "c1"},
                          {"e", 1, kTypeValue, "e1"},
                      }));
  auto t2 = MakeTable(icmp, "iter_m2.ldb",
                      BuildEntries({
                          {"b", 1, kTypeValue, "b1"},
                          {"d", 1, kTypeValue, "d1"},
                          {"f", 1, kTypeValue, "f1"},
                      }));

  DBIterator it(&icmp, 100);
  it.AddChild(t1->NewIterator());
  it.AddChild(t2->NewIterator());

  std::vector<KV> want = {{"a", "a1"}, {"b", "b1"}, {"c", "c1"},
                          {"d", "d1"}, {"e", "e1"}, {"f", "f1"}};
  EXPECT_EQ(Collect(&it), want);
}

// 同一个 key 同时存在于新旧两个源：只输出一次，取更新的版本。
TEST(IteratorTest, MergingIteratorResolvesDuplicateAcrossSources) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto old_t = MakeTable(icmp, "iter_dup_old.ldb",
                         BuildEntries({
                             {"k", 1, kTypeValue, "from_sst"},
                             {"only_old", 1, kTypeValue, "o"},
                         }));
  auto new_t = MakeTable(icmp, "iter_dup_new.ldb",
                         BuildEntries({{"k", 50, kTypeValue, "from_new"}}));

  DBIterator it(&icmp, 100);
  it.AddChild(old_t->NewIterator());
  it.AddChild(new_t->NewIterator());

  std::vector<KV> want = {{"k", "from_new"}, {"only_old", "o"}};
  EXPECT_EQ(Collect(&it), want);
}

// 墓碑与旧值分处两个源时，可见性判定必须**只按 sequence 决定**，与来源无关。
// 这条测的是归并器的正确性核心：不能因为"值在旧文件、墓碑在新文件"就搞反。
TEST(IteratorTest, MergingIteratorTombstoneWinsBySequence) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto value_src =
      MakeTable(icmp, "iter_tw_v.ldb",
                BuildEntries({{"x", 5, kTypeValue, "alive"}}));
  auto tomb_src =
      MakeTable(icmp, "iter_tw_t.ldb",
                BuildEntries({{"x", 9, kTypeDeletion, ""}}));

  DBIterator it(&icmp, 100);
  it.AddChild(value_src->NewIterator());
  it.AddChild(tomb_src->NewIterator());

  EXPECT_TRUE(Collect(&it).empty()) << "seq9 的墓碑必须遮挡 seq5 的值";
}

// Seek：定位到第一个 >= target 的 user_key，并支持 Seek 与 Next 交替。
TEST(IteratorTest, MergingIteratorSeek) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_seek.ldb",
                         BuildEntries({
                             {"a", 1, kTypeValue, "1"},
                             {"c", 1, kTypeValue, "3"},
                             {"e", 1, kTypeValue, "5"},
                         }));

  DBIterator it(&icmp, 100);
  it.AddChild(table->NewIterator());

  it.Seek("c");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("c", it.key().ToString());

  // 落在两个 key 之间 -> 落到下一个
  it.Seek("cc");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("e", it.key().ToString());

  // 小于全部 -> 第一个
  it.Seek("0");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("a", it.key().ToString());

  // 超过全部 -> 无效
  it.Seek("z");
  EXPECT_FALSE(it.Valid());

  // Seek 之后可以继续 Next
  it.Seek("a");
  ASSERT_TRUE(it.Valid());
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("c", it.key().ToString());
}

// Seek 到比当前位置更小的 key（回退）必须同样正确——归并器不能有"单向游标"
// 之类的隐藏状态。
TEST(IteratorTest, MergingIteratorSeekBackward) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_seek_bw.ldb",
                         BuildEntries({
                             {"a", 1, kTypeValue, "1"},
                             {"m", 1, kTypeValue, "2"},
                             {"z", 1, kTypeValue, "3"},
                         }));

  DBIterator it(&icmp, 100);
  it.AddChild(table->NewIterator());

  it.Seek("z");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("z", it.key().ToString());

  it.Seek("a");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("a", it.key().ToString());
}

// key()/value() 生命周期契约：返回内部拷贝，移动位置后旧值不受影响。
// 若改成引用子迭代器的缓冲区，这里会读到被覆写的数据。
TEST(IteratorTest, KeyValueSurviveNext) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto table = MakeTable(icmp, "iter_life.ldb",
                         BuildEntries({
                             {"k1", 1, kTypeValue, "v1"},
                             {"k2", 1, kTypeValue, "v2"},
                         }));

  DBIterator it(&icmp, 100);
  it.AddChild(table->NewIterator());
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());

  const std::string saved_key = it.key().ToString();
  const std::string saved_val = it.value().ToString();

  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("k2", it.key().ToString());
  EXPECT_EQ("v1", saved_val) << "先前取得的 value 不应被后续 Next 破坏";
  EXPECT_EQ("k1", saved_key);
}

// 空数据源与"完全没有数据源"都必须安全
TEST(IteratorTest, MergingIteratorWithEmptySources) {
  InternalKeyComparator icmp(BytewiseComparator());
  auto empty = MakeTable(icmp, "iter_e2.ldb", {});
  auto nonempty =
      MakeTable(icmp, "iter_ne.ldb", BuildEntries({{"x", 1, kTypeValue, "vx"}}));

  // 一个空源 + 一个有数据的源
  DBIterator mixed(&icmp, 100);
  mixed.AddChild(empty->NewIterator());
  mixed.AddChild(nonempty->NewIterator());
  std::vector<KV> want = {{"x", "vx"}};
  EXPECT_EQ(Collect(&mixed), want);

  // 全部为空
  DBIterator all_empty(&icmp, 100);
  all_empty.AddChild(empty->NewIterator());
  all_empty.SeekToFirst();
  EXPECT_FALSE(all_empty.Valid());
  EXPECT_TRUE(all_empty.status().ok());
}

TEST(IteratorTest, MergingIteratorWithNoSources) {
  InternalKeyComparator icmp(BytewiseComparator());
  DBIterator it(&icmp, 100);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek("anything");
  EXPECT_FALSE(it.Valid());
  it.Next();  // 无效状态下调用 Next 必须安全
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
}

}  // namespace
}  // namespace tinystore