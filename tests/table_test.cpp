#include <algorithm>
#include <cstdint>
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

// 故障注入桩：第 fail_at 次 Append（0 基）失败，其余写入都成功。
// TableBuilder 直接接受 WritableFile*（不经过 Env），所以实现这一个接口就够，
// 不用去写一整套 TestEnv。
//
// 【为什么只让某一次失败、其余成功 —— 这个细节决定测试有没有意义】
// 如果让 Append 全部失败，那么连最后写 footer 的那次 Append 也会失败，
// status_ 恰好被置错，Finish() 自然返回错误 —— 就算中间块的错误根本没被
// 传播，测试也会"通过"，属于假阳性。
// 真实的缺陷 #1 恰恰是：中间块写失败被丢弃，随后 footer 写入成功并把
// status_ 覆盖成 OK，Finish() 于是错误地报告成功。所以这里必须让中间块
// 失败、footer 成功，才能真正区分"传播了"与"被吞了"。
class FailingWritableFile : public WritableFile {
public:
  explicit FailingWritableFile(int fail_at) : fail_at_(fail_at) {}

  Status Append(const Slice& data) override {
    (void)data;  // 桩类不关心写入内容，只按调用次数决定何时失败
    if (calls_++ == fail_at_) {
      return Status::IOError("injected write failure");
    }
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

private:
  int fail_at_;
  int calls_ = 0;
};

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

// ---------------------------------------------------------------------------
// 回归测试：写入错误必须向上传播（对应缺陷 #1）
//
// 背景：TableBuilder 过去会丢弃 WriteBlock / Append 的返回值，只有写 footer 时
// 才设置 status_。于是磁盘满这类故障下 Finish() 依然返回 OK，上层会把一个缺块
// 截断的 SSTable 登记进 MANIFEST 并删掉旧 WAL —— 数据静默丢失且无任何报错。
// ---------------------------------------------------------------------------
TEST(TableTest, BuilderPropagatesWriteFailureOnFinish) {
  InternalKeyComparator icmp(BytewiseComparator());
  // 第 0 次 Append（第一个 data block）失败，后面的 meta/index/footer 写入都成功。
  FailingWritableFile file(/*fail_at=*/0);
  TableBuilder builder(&icmp, &file, /*filter_policy=*/nullptr,
                       /*block_size=*/4096);

  builder.Add(IK("k1", 1, kTypeValue), "v1");

  // block_size 足够大，数据块要到 Finish 才落盘 -> 错误在 Finish 暴露。
  Status s = builder.Finish();
  // 关键：footer 的写入是成功的。若中间块的失败没有被传播，status_ 会被
  // footer 的成功覆盖成 OK，Finish() 就错误地报告成功（缺陷 #1 的表现）。
  EXPECT_FALSE(s.ok()) << "Finish 必须上报中间块的写失败，不能被 footer 成功覆盖";
  EXPECT_TRUE(s.IsIOError()) << "actual: " << s.ToString();
}

TEST(TableTest, BuilderStopsAfterWriteFailureDuringAdd) {
  InternalKeyComparator icmp(BytewiseComparator());
  FailingWritableFile file(/*fail_at=*/0);
  // block_size=1 让每次 Add 都立刻切块落盘 -> 错误在 Add 期间就发生。
  TableBuilder builder(&icmp, &file, /*filter_policy=*/nullptr,
                       /*block_size=*/1);

  builder.Add(IK("k1", 1, kTypeValue), "v1");  // 内部 Flush -> 第 0 次 Append 失败
  // 关键：出错后继续 Add 必须是 no-op（status_ 已置错）。如果后续 Add 还往文件里
  // 写东西，就等于在一个已经损坏的构建过程上叠加内容，问题更难定位。
  builder.Add(IK("k2", 2, kTypeValue), "v2");

  Status s = builder.Finish();
  EXPECT_FALSE(s.ok()) << "Add 期间的写失败必须让整个 builder 作废";
  EXPECT_TRUE(s.IsIOError()) << "actual: " << s.ToString();
}

// ---------------------------------------------------------------------------
// 回归测试：越界 BlockHandle 必须被拒绝（对应缺陷 #3）
//
// 背景：ReadBlock 过去用 offset + size + trailer > file_size 做边界检查。
// 三个 uint64 相加会回绕，一个接近 UINT64_MAX 的 offset 就能让这个判断失效，
// 随后带着超大偏移发起 pread。现在改为无溢出的分步比较。
// ---------------------------------------------------------------------------
TEST(TableTest, OpenRejectsOutOfRangeBlockHandle) {
  InternalKeyComparator icmp(BytewiseComparator());

  // 手工构造一个"只有 footer"的最小 SSTable：magic 合法，但 index handle
  // 指向文件之外，且取值接近 UINT64_MAX —— 正是旧的加法比较会被绕过的情形。
  Footer footer;
  footer.meta_index_handle = BlockHandle{0, 0};
  footer.index_handle.offset = UINT64_MAX - 1;
  footer.index_handle.size = UINT64_MAX;
  std::string content;
  footer.EncodeTo(&content);

  std::string path;
  Env::Default()->GetTestDirectory(&path);
  path += "/table_bad_handle.ldb";
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(Env::Default()->NewWritableFile(path, &file).ok());
    ASSERT_TRUE(file->Append(content).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  std::unique_ptr<RandomAccessFile> rf;
  ASSERT_TRUE(Env::Default()->NewRandomAccessFile(path, &rf).ok());
  uint64_t size = 0;
  ASSERT_TRUE(Env::Default()->GetFileSize(path, &size).ok());

  Table* table = nullptr;
  Status s = Table::Open(&icmp, std::move(rf), size, &table);
  // 必须报 Corruption：既不能崩溃，也不能"成功打开一个空表"把问题藏起来。
  EXPECT_TRUE(s.IsCorruption()) << "actual: " << s.ToString();
  EXPECT_EQ(table, nullptr);
}

}  // namespace
}  // namespace tinystore
