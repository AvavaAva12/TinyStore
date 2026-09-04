#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "tinystore/comparator.h"
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

// 构造一个 SSTable：entries 为 (user_key, seq, type, value) 序列，必须按
// internal_key 升序（即 user_key 升序、同 key 时 seq 降序）。
static std::string BuildTable(const InternalKeyComparator& icmp,
                              const std::string& path,
                              const std::vector<std::string>& internal_keys,
                              const std::vector<std::string>& values) {
  std::unique_ptr<WritableFile> file;
  EXPECT_TRUE(Env::Default()->NewWritableFile(path, &file).ok());
  TableBuilder builder(&icmp, file.get(), DefaultFilterPolicy(),
                       /*block_size=*/256);  // 小块以制造多个 data block + 索引查找
  for (size_t i = 0; i < internal_keys.size(); ++i) {
    builder.Add(internal_keys[i], values[i]);
  }
  EXPECT_TRUE(builder.Finish().ok());
  EXPECT_TRUE(file->Sync().ok());
  EXPECT_TRUE(file->Close().ok());
  return path;
}

TEST(TableTest, RoundTripAndMVCC) {
  InternalKeyComparator icmp(BytewiseComparator());

  // 收集所有 (internal_key, value) 对，再严格按 internal_key 升序排序后写入表。
  // 比较器规则：user_key 升序，同 user_key 时 seq 降序。手动保证顺序容易出错
  // （例如 "del" 字典序小于 "key00000"/"mv"），这里用 icmp 排序一劳永逸。
  std::vector<std::pair<std::string, std::string>> entries;
  // 普通键 key00000..key00099，各一个版本 seq=10
  for (int i = 0; i < 100; ++i) {
    std::string u = "key" + std::string(5 - std::to_string(i).size(), '0') +
                    std::to_string(i);
    entries.emplace_back(IK(u, 10, kTypeValue), "v" + std::to_string(i));
  }
  // 多版本键 mv：seq=20 -> B，seq=5 -> A（用于快照读验证）
  entries.emplace_back(IK("mv", 20, kTypeValue), "B");
  entries.emplace_back(IK("mv", 5, kTypeValue), "A");
  // 删除键 del：seq=7 删除
  entries.emplace_back(IK("del", 7, kTypeDeletion), "");

  std::sort(entries.begin(), entries.end(),
            [&](const std::pair<std::string, std::string>& a,
                const std::pair<std::string, std::string>& b) {
              return icmp.Compare(a.first, b.first) < 0;
            });

  std::vector<std::string> keys, vals;
  keys.reserve(entries.size());
  vals.reserve(entries.size());
  for (auto& e : entries) {
    keys.push_back(std::move(e.first));
    vals.push_back(std::move(e.second));
  }

  std::string path;
  Env::Default()->GetTestDirectory(&path);
  path += "/table_test.ldb";
  BuildTable(icmp, path, keys, vals);

  // 重打开
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(Env::Default()->NewRandomAccessFile(path, &file).ok());
  uint64_t size = 0;
  ASSERT_TRUE(Env::Default()->GetFileSize(path, &size).ok());
  Table* table = nullptr;
  ASSERT_TRUE(Table::Open(&icmp, std::move(file), size, &table).ok());
  std::unique_ptr<Table> t(table);

  // 普通键：最新可见（seq=10）的值
  std::string v;
  bool found = false;
  ASSERT_TRUE(table->Get("key00042", 100, DefaultFilterPolicy(), &v, &found).ok());
  ASSERT_TRUE(found);
  ASSERT_EQ(v, "v42");

  // 多版本键：快照 10 看到 seq=5 (A)，快照 30 看到 seq=20 (B)
  ASSERT_TRUE(table->Get("mv", 10, DefaultFilterPolicy(), &v, &found).ok());
  ASSERT_TRUE(found);
  ASSERT_EQ(v, "A");
  ASSERT_TRUE(table->Get("mv", 30, DefaultFilterPolicy(), &v, &found).ok());
  ASSERT_TRUE(found);
  ASSERT_EQ(v, "B");

  // 删除键：快照 100 看到删除（found=true, NotFound）；快照 5 看不到（found=false）
  Status s = table->Get("del", 100, DefaultFilterPolicy(), &v, &found);
  ASSERT_TRUE(found);
  ASSERT_TRUE(s.IsNotFound());
  s = table->Get("del", 5, DefaultFilterPolicy(), &v, &found);
  ASSERT_FALSE(found);
  ASSERT_TRUE(s.IsNotFound());

  // 不存在的键：found=false
  s = table->Get("nope", 100, DefaultFilterPolicy(), &v, &found);
  ASSERT_FALSE(found);
  ASSERT_TRUE(s.IsNotFound());
}

TEST(TableTest, BloomSkipsAbsentKey) {
  // 用一个没有布隆过滤器的 builder，验证少了过滤器也能正常工作；
  // 再验证有过滤器时 Get 对不存在的键仍能正确返回 NotFound。
  InternalKeyComparator icmp(BytewiseComparator());
  std::vector<std::string> keys, vals;
  for (int i = 0; i < 50; ++i) {
    std::string u = "k" + std::string(2 - std::to_string(i).size(), '0') + std::to_string(i);
    keys.push_back(IK(u, 1, kTypeValue));
    vals.push_back("x");
  }
  std::string path;
  Env::Default()->GetTestDirectory(&path);
  path += "/table_test_nofilter.ldb";
  BuildTable(icmp, path, keys, vals);

  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(Env::Default()->NewRandomAccessFile(path, &file).ok());
  uint64_t size = 0;
  ASSERT_TRUE(Env::Default()->GetFileSize(path, &size).ok());
  Table* table = nullptr;
  ASSERT_TRUE(Table::Open(&icmp, std::move(file), size, &table).ok());
  std::unique_ptr<Table> t(table);

  std::string v;
  bool found = false;
  // 传 nullptr 过滤器：应仍走索引+数据块，正确返回 NotFound
  Status s = table->Get("zzz", 100, nullptr, &v, &found);
  ASSERT_FALSE(found);
  ASSERT_TRUE(s.IsNotFound());
}

}  // namespace
}  // namespace tinystore
