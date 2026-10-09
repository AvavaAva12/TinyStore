#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "tinystore/db.h"
#include "tinystore/write_batch.h"
#include "gtest/gtest.h"

namespace tinystore {
namespace {

// 每个测试用独立的子目录，避免相互污染；测试结束清理。
static std::string TempDbName(const std::string& sub) {
  std::string dir;
  Env::Default()->GetTestDirectory(&dir);
  return dir + "/" + sub;
}

static void RemoveAll(const std::string& name) {
  std::error_code ec;
  std::filesystem::remove_all(name, ec);
}

TEST(DBTest, PutGetMissing) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("putget");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_NE(db, nullptr);

  std::string got;
  ASSERT_TRUE(db->Get("nope", &got).IsNotFound());

  ASSERT_TRUE(db->Put("foo", "bar").ok());
  ASSERT_TRUE(db->Get("foo", &got).ok());
  ASSERT_EQ(got, "bar");

  // 另一个不存在的 key 仍 NotFound
  ASSERT_TRUE(db->Get("nope2", &got).IsNotFound());

  delete db;
  RemoveAll(name);
}

TEST(DBTest, OverwriteReturnsLatest) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("overwrite");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  ASSERT_TRUE(db->Put("k", "v1").ok());
  ASSERT_TRUE(db->Put("k", "v2").ok());
  std::string got;
  ASSERT_TRUE(db->Get("k", &got).ok());
  ASSERT_EQ(got, "v2");  // MVCC 快照读返回最新版本

  delete db;
  RemoveAll(name);
}

TEST(DBTest, DeleteTombstone) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("delete");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  ASSERT_TRUE(db->Put("k", "v").ok());
  ASSERT_TRUE(db->Delete("k").ok());
  std::string got;
  ASSERT_TRUE(db->Get("k", &got).IsNotFound());  // 墓碑 -> NotFound

  // 删除之后再写回来，应能读到新值
  ASSERT_TRUE(db->Put("k", "v2").ok());
  ASSERT_TRUE(db->Get("k", &got).ok());
  ASSERT_EQ(got, "v2");

  delete db;
  RemoveAll(name);
}

TEST(DBTest, WriteBatchMultiple) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("writebatch");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");  // 同 batch 内先写后删
  ASSERT_TRUE(db->Write(batch).ok());

  std::string got;
  ASSERT_TRUE(db->Get("a", &got).IsNotFound());  // 被同 batch 的 Delete 抵消
  ASSERT_TRUE(db->Get("b", &got).ok());
  ASSERT_EQ(got, "2");

  delete db;
  RemoveAll(name);
}

TEST(DBTest, LargeValueSpansBlocks) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("large");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 100KB 的 value，远超 WAL 的 32KB 块，必须被切成多个分片
  const std::string big(100 * 1024, 'x');
  ASSERT_TRUE(db->Put("big", big).ok());
  std::string got;
  ASSERT_TRUE(db->Get("big", &got).ok());
  ASSERT_EQ(got, big);

  delete db;
  RemoveAll(name);
}

// ---- 崩溃恢复：重放 WAL 把 MemTable 重建出来 ----
TEST(DBTest, RecoverAfterReopen) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("recover");
  RemoveAll(name);

  {
    DB* db;
    ASSERT_TRUE(DB::Open(opt, name, &db).ok());
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Put("key" + std::to_string(i),
                          "val" + std::to_string(i)).ok());
    }
    // 包含一个删除，验证墓碑也能被正确重放
    ASSERT_TRUE(db->Delete("key" + std::to_string(50)).ok());
    delete db;  // 析构会 Sync + Close WAL
  }

  // 重新打开同一目录：应通过重放 WAL 恢复数据
  {
    DB* db2;
    ASSERT_TRUE(DB::Open(opt, name, &db2).ok());
    for (int i = 0; i < 200; ++i) {
      std::string got;
      const std::string key = "key" + std::to_string(i);
      if (i == 50) {
        ASSERT_TRUE(db2->Get(key, &got).IsNotFound());
      } else {
        ASSERT_TRUE(db2->Get(key, &got).ok());
        ASSERT_EQ(got, "val" + std::to_string(i));
      }
    }
    delete db2;
  }

  RemoveAll(name);
}

// ---- 并发写 + 并发读，验证 Group Commit 与无锁读在 TSAN 下无数据竞争 ----
TEST(DBTest, ConcurrentWritesAndReads) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("concurrent");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  const int nthreads = 8;
  const int per_thread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < nthreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < per_thread; ++i) {
        const std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
        const std::string val = "v" + std::to_string(i);
        EXPECT_TRUE(db->Put(Slice(key), Slice(val)).ok());
        // 并发读自己刚写的值（无锁读路径，TSAN 重点覆盖）
        std::string got;
        EXPECT_TRUE(db->Get(Slice(key), &got).ok());
        EXPECT_EQ(got, val);
      }
    });
  }
  for (auto& th : threads) th.join();

  // 最终一致性校验：所有 key 都应可读到正确值
  for (int t = 0; t < nthreads; ++t) {
    for (int i = 0; i < per_thread; ++i) {
      const std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
      const std::string val = "v" + std::to_string(i);
      std::string got;
      ASSERT_TRUE(db->Get(Slice(key), &got).ok()) << "missing " << key;
      ASSERT_EQ(got, val);
    }
  }

  delete db;
  RemoveAll(name);
}

TEST(DBTest, EmptyWriteIsNoop) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("empty");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  WriteBatch empty;
  ASSERT_TRUE(db->Write(empty).ok());  // 空 batch 不应崩溃 / 不应写 WAL
  std::string got;
  ASSERT_TRUE(db->Get("anything", &got).IsNotFound());

  delete db;
  RemoveAll(name);
}

// ---- W4：flush 把 MemTable 落盘成 SSTable；重开后数据应从 SSTable 读出 ----

// 用极小的 write_buffer_size 强制触发 flush，写入远超缓冲的数据，
// 关闭后重新打开，验证所有数据仍在（且至少部分已落在 SSTable 而非仅 WAL）。
TEST(DBTest, FlushPersistsAcrossReopen) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;  // 1KB：几个写后就触发 flush
  const std::string name = TempDbName("flush");
  RemoveAll(name);

  {
    DB* db;
    ASSERT_TRUE(DB::Open(opt, name, &db).ok());
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
    }
    delete db;
  }
  {
    DB* db2;
    ASSERT_TRUE(DB::Open(opt, name, &db2).ok());
    for (int i = 0; i < 2000; ++i) {
      std::string got;
      ASSERT_TRUE(db2->Get("k" + std::to_string(i), &got).ok()) << "missing " << i;
      ASSERT_EQ(got, "v" + std::to_string(i));
    }
    // 删除一个键，再重开仍应读不到（墓碑也要能持久化）
    ASSERT_TRUE(db2->Delete("k" + std::to_string(1234)).ok());
    std::string got;
    ASSERT_TRUE(db2->Get("k" + std::to_string(1234), &got).IsNotFound());
    delete db2;
  }
  RemoveAll(name);
}

// flush 之后，新写的数据走新的 WAL + 新 MemTable，旧数据在 SSTable；
// 混合读写验证"MemTable 最新 + SSTable 兜底"的合并读正确。
TEST(DBTest, ReadSpanningMemTableAndSSTable) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;
  const std::string name = TempDbName("mixed");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 第一组：写足触发 flush，全部进入 SSTable
  for (int i = 0; i < 1500; ++i) {
    ASSERT_TRUE(db->Put("old" + std::to_string(i), "o" + std::to_string(i)).ok());
  }
  // 第二组：再写一批（部分仍在 MemTable，部分又 flush）
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put("new" + std::to_string(i), "n" + std::to_string(i)).ok());
  }
  // 覆盖一个 old 键，验证 SSTable 里的旧版本被 MemTable 里的新版本盖过
  ASSERT_TRUE(db->Put("old777", "OVERWRITTEN").ok());

  for (int i = 0; i < 1500; ++i) {
    std::string got;
    ASSERT_TRUE(db->Get("old" + std::to_string(i), &got).ok()) << "old missing " << i;
  }
  std::string overwritten;
  ASSERT_TRUE(db->Get("old777", &overwritten).ok());
  ASSERT_EQ(overwritten, "OVERWRITTEN");
  for (int i = 0; i < 500; ++i) {
    std::string got;
    ASSERT_TRUE(db->Get("new" + std::to_string(i), &got).ok()) << "new missing " << i;
  }

  delete db;
  RemoveAll(name);
}

TEST(DBTest, ConcurrentWritesWithFlushes) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 4096;
  const std::string name = TempDbName("concflush");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  const int nthreads = 8;
  const int per_thread = 300;
  std::vector<std::thread> threads;
  for (int t = 0; t < nthreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < per_thread; ++i) {
        const std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
        const std::string val = "v" + std::to_string(i);
        EXPECT_TRUE(db->Put(Slice(key), Slice(val)).ok());
        std::string got;
        EXPECT_TRUE(db->Get(Slice(key), &got).ok());
        EXPECT_EQ(got, val);
      }
    });
  }
  for (auto& th : threads) th.join();

  for (int t = 0; t < nthreads; ++t) {
    for (int i = 0; i < per_thread; ++i) {
      const std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
      std::string got;
      ASSERT_TRUE(db->Get(Slice(key), &got).ok()) << "missing " << key;
      ASSERT_EQ(got, "v" + std::to_string(i));
    }
  }

  // 重开验证持久化（覆盖 flush 路径）
  delete db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  for (int t = 0; t < nthreads; ++t) {
    for (int i = 0; i < per_thread; ++i) {
      const std::string key = "k" + std::to_string(t) + "_" + std::to_string(i);
      std::string got;
      ASSERT_TRUE(db->Get(Slice(key), &got).ok()) << "missing after reopen " << key;
      ASSERT_EQ(got, "v" + std::to_string(i));
    }
  }

  delete db;
  RemoveAll(name);
}

// ---- W5：迭代器（范围扫描）端到端 ----

// 把迭代器输出收集成 vector<(key, value)>
static std::vector<std::pair<std::string, std::string>> ScanAll(
    DB* db, const ReadOptions& options = ReadOptions()) {
  std::vector<std::pair<std::string, std::string>> out;
  std::unique_ptr<Iterator> it(db->NewIterator(options));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.emplace_back(it->key().ToString(), it->value().ToString());
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  return out;
}

TEST(DBTest, IteratorScansWholeDatabase) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("iterscan");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_TRUE(db->Put("apple", "1").ok());
  ASSERT_TRUE(db->Put("banana", "2").ok());
  ASSERT_TRUE(db->Put("cherry", "3").ok());

  auto got = ScanAll(db);
  std::vector<std::pair<std::string, std::string>> want = {
      {"apple", "1"}, {"banana", "2"}, {"cherry", "3"}};
  EXPECT_EQ(got, want);

  delete db;
  RemoveAll(name);
}

// 遍历必须能跨 MemTable 与 SSTable 两个来源：极小的 write_buffer_size 让每次
// Put 都 flush，数据落在 SSTable；两者需被正确归并且不重不漏。
TEST(DBTest, IteratorSpansMemTableAndSSTables) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;  // 触发 flush
  const std::string name = TempDbName("iterspan");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  const int n = 300;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i),
                        "v" + std::to_string(i)).ok())
        << i;
  }

  auto got = ScanAll(db);
  ASSERT_EQ(got.size(), static_cast<size_t>(n));

  // 迭代器按 user_key 的**字典序**输出，不是数值序——"key10" < "key2"
  // （'1' < '2'）。因此不能拿第 i 个元素去对第 i 个 key，而要校验三件事：
  //   1. 输出严格字典序递增（有序）
  //   2. key 集合完整、无遗漏无重复
  //   3. 每个 key 的值与 Get 一致（迭代视图 == 点查视图）
  for (size_t i = 1; i < got.size(); ++i) {
    EXPECT_LT(BytewiseComparator()->Compare(got[i - 1].first, got[i].first), 0)
        << "iteration must be strictly ascending at index " << i;
  }

  std::vector<std::string> want_keys;
  for (int i = 0; i < n; ++i) {
    const std::string k = "key" + std::to_string(i);
    want_keys.push_back(k);
    std::string v;
    ASSERT_TRUE(db->Get(k, &v).ok()) << k;
    EXPECT_EQ("v" + std::to_string(i), v) << k;
  }
  std::sort(want_keys.begin(), want_keys.end(),
            [](const std::string& a, const std::string& b) {
              return BytewiseComparator()->Compare(a, b) < 0;
            });

  std::vector<std::string> got_keys;
  got_keys.reserve(got.size());
  for (const auto& kv : got) got_keys.push_back(kv.first);
  EXPECT_EQ(got_keys, want_keys);

  delete db;
  RemoveAll(name);
}

// 迭代器视图必须与 Get 一致；删除的 key 在迭代中不出现。
TEST(DBTest, IteratorSkipsDeletedAndMatchesGet) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;
  const std::string name = TempDbName("iterdel");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), "v").ok());
  }
  ASSERT_TRUE(db->Delete("k0").ok());
  ASSERT_TRUE(db->Delete("k50").ok());
  ASSERT_TRUE(db->Delete("k99").ok());

  auto got = ScanAll(db);
  EXPECT_EQ(got.size(), 97u);
  for (const auto& kv : got) {
    const std::string& k = kv.first;
    EXPECT_NE("k0", k);
    EXPECT_NE("k50", k);
    EXPECT_NE("k99", k);
    std::string v;
    ASSERT_TRUE(db->Get(k, &v).ok()) << k;
    EXPECT_EQ(kv.second, v) << k;
  }

  delete db;
  RemoveAll(name);
}

// 同一 key 多次写入：迭代只应出现一次，且是最新值。
TEST(DBTest, IteratorYieldsLatestVersionOnlyOnce) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;
  const std::string name = TempDbName("iterdup");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(db->Put("hot", "v" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db->Put("other", "x").ok());

  auto got = ScanAll(db);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ("hot", got[0].first);
  EXPECT_EQ("v19", got[0].second) << "应取最新版本";
  EXPECT_EQ("other", got[1].first);

  delete db;
  RemoveAll(name);
}

// Seek + Next：从中间开始扫到末尾。
TEST(DBTest, IteratorSeekAndNext) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("itersk");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("c", "3").ok());
  ASSERT_TRUE(db->Put("e", "5").ok());

  std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
  it->Seek("c");
  std::vector<std::string> seen;
  for (; it->Valid(); it->Next()) seen.push_back(it->key().ToString());
  std::vector<std::string> want = {"c", "e"};
  EXPECT_EQ(seen, want);

  // Seek 越过末尾 -> 无效，但不崩
  it->Seek("zzz");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());

  delete db;
  RemoveAll(name);
}

// 空库：迭代器立即无效，不崩。
TEST(DBTest, IteratorOnEmptyDatabase) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("iterempty");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
  it->Seek("anything");
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(ScanAll(db).empty());

  delete db;
  RemoveAll(name);
}

// 快照读：旧快照视图不应包含快照之后写入的数据。
// 这是迭代器"时间旅行"能力的端到端验证——ReadOptions.snapshot 固定后，
// 后续写入对该视图不可见，且遍历结果保持一致。
TEST(DBTest, IteratorHonorsSnapshot) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("itersnap");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_TRUE(db->Put("a", "old").ok());  // 第一条写入占用 sequence 1

  ReadOptions snap;
  snap.snapshot = 1;  // 固定"只含 sequence <= 1"的视图
  ASSERT_TRUE(db->Put("b", "new").ok());

  // 迭代器必须先于 DB 销毁（它借用了 VersionSet 的 Table 引用）
  {
    std::unique_ptr<Iterator> old_it(db->NewIterator(snap));
    old_it->SeekToFirst();
    ASSERT_TRUE(old_it->Valid());
    EXPECT_EQ("a", old_it->key().ToString());
    EXPECT_EQ("old", old_it->value().ToString());
    old_it->Next();
    EXPECT_FALSE(old_it->Valid())
        << "快照之后写入的 b 不应出现在旧快照视图中";
  }

  // 默认读最新：a、b 都在
  auto now = ScanAll(db);
  ASSERT_EQ(now.size(), 2u);
  EXPECT_EQ("b", now[1].first);
  EXPECT_EQ("new", now[1].second);

  delete db;
  RemoveAll(name);
}

// ---- W6：Compaction ----

// 让写入产生大量 flush：用极小的 write_buffer_size，每个 MemTable 很快写满。
static Options TinyBufferOptions(const std::string& sub) {
  (void)sub;
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;
  return opt;
}

// 后台 compaction 是异步的：写完最后一个 key 时它可能还在跑。
// 断言文件数之前必须先等它收敛，否则测试会随机失败（flaky）。
// 这里轮询而不是 sleep 固定时长——固定 sleep 会让慢机器上的测试莫名通过。
static bool WaitForFileCountAtMost(DB* db, size_t limit, int timeout_ms) {
  const int step = 5;
  for (int waited = 0; waited <= timeout_ms; waited += step) {
    if (db->NumTableFiles() <= limit) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
  }
  return db->NumTableFiles() <= limit;
}

// 核心收益：文件数必须收敛，而不是随写入量线性增长。
// 这是 Compaction 存在的唯一理由，务必直接断言。
TEST(DBTest, CompactionConvergesFileCount) {
  Options opt = TinyBufferOptions("compact1");
  opt.l0_compaction_trigger = 4;
  const std::string name = TempDbName("compact1");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
  }

  // 每次写满 MemTable 都会 flush 出 1 个 L0 文件，2000 次写会产生上千个文件。
  // 若 compaction 生效，L0 会被压在阈值内，大部分数据沉到 L1，总文件数应远小于 n。
  //
  // compaction 在后台线程跑，所以先等它收敛再断言。
  ASSERT_TRUE(WaitForFileCountAtMost(db, static_cast<size_t>(n) / 2, 30000))
      << "compaction did not converge file count: " << db->NumTableFiles();
  EXPECT_LT(db->NumTableFiles(), static_cast<size_t>(n) / 2);
  EXPECT_LE(db->GetLevelFileCounts()[0], opt.l0_compaction_trigger)
      << "L0 should stay under the trigger threshold";

  // 数据必须一条不少、值不能错
  for (int i = 0; i < n; ++i) {
    const std::string k = "k" + std::to_string(i);
    std::string v;
    ASSERT_TRUE(db->Get(k, &v).ok()) << "missing after compaction: " << k;
    EXPECT_EQ("v" + std::to_string(i), v) << k;
  }

  delete db;
  RemoveAll(name);
}

// 归并必须保留最新版本：同一个 key 反复覆盖后只应留下最后一次的值。
TEST(DBTest, CompactionKeepsLatestVersion) {
  Options opt = TinyBufferOptions("compact2");
  opt.l0_compaction_trigger = 4;
  const std::string name = TempDbName("compact2");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 少量 key 反复写，跨过多次 compaction 边界
  const int rounds = 200;
  for (int i = 0; i < rounds; ++i) {
    for (int k = 0; k < 20; ++k) {
      ASSERT_TRUE(db->Put("hot" + std::to_string(k),
                          "round" + std::to_string(i)).ok());
    }
  }
  for (int k = 0; k < 20; ++k) {
    std::string v;
    ASSERT_TRUE(db->Get("hot" + std::to_string(k), &v).ok());
    EXPECT_EQ("round" + std::to_string(rounds - 1), v);
  }

  delete db;
  RemoveAll(name);
}

// 墓碑必须在 compaction 中被真正回收：删掉的 key 不应继续占用空间。
// 若墓碑没被丢掉，被删 key 的旧值会在某次压缩后"复活"。
TEST(DBTest, CompactionReclaimsTombstones) {
  Options opt = TinyBufferOptions("compact3");
  opt.l0_compaction_trigger = 4;
  const std::string name = TempDbName("compact3");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 写入一批数据后全部删除，再写入更多数据触发多轮 compaction
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put("gone" + std::to_string(i), "x").ok());
  }
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Delete("gone" + std::to_string(i)).ok());
  }
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put("stay" + std::to_string(i), "y").ok());
  }

  // 删除的 key 必须真的消失（否则说明墓碑被丢掉得太早、或旧值复活）
  for (int i = 0; i < 500; ++i) {
    std::string v;
    EXPECT_TRUE(db->Get("gone" + std::to_string(i), &v).IsNotFound())
        << "deleted key came back: gone" << i;
  }
  // 保留的 key 必须完好
  for (int i = 0; i < 500; ++i) {
    std::string v;
    ASSERT_TRUE(db->Get("stay" + std::to_string(i), &v).ok()) << i;
    EXPECT_EQ("y", v);
  }

  // 墓碑回收后，总字节数不应随"写了又删"的总量线性膨胀。
  // 这里只做宽松断言：删掉 500 个 key 后，L1 体积应明显小于同时保留 1000 个 key。
  const auto bytes = db->GetLevelBytes();
  uint64_t total = 0;
  for (size_t b : bytes) total += b;
  EXPECT_LT(total, 4u << 20) << "tombstones were not reclaimed, bytes=" << total;

  delete db;
  RemoveAll(name);
}

// 全部 key 被删光时，compaction 不得产出空 SSTable。
TEST(DBTest, CompactionOfFullyDeletedSetProducesNoFiles) {
  Options opt = TinyBufferOptions("compact4");
  opt.l0_compaction_trigger = 4;
  const std::string name = TempDbName("compact4");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), "v").ok());
    ASSERT_TRUE(db->Delete("k" + std::to_string(i)).ok());
  }

  // 数据全没了，被 tombstone 占住的空间应被 compaction 回收。
  //
  // 这里允许残留少量文件：末尾若还有没写满的 MemTable，要等下一次写入才会
  // flush 并触发下一轮 compaction。只要被删的数据不再以旧值形式留在磁盘上，
  // compaction 的目的就达到了。
  const size_t left = db->NumTableFiles();
  EXPECT_LE(left, 2u) << "fully-deleted data should be reclaimed, left " << left
                      << " files";

  // 后台 compaction 可能仍在收尾，等它跑完再看最终的文件数
  WaitForFileCountAtMost(db, 2u, 10000);

  // 更要紧的是：这些 key 既读不到，也不会在后续 compaction 中复活。
  for (int i = 0; i < 200; ++i) {
    std::string v;
    EXPECT_TRUE(db->Get("k" + std::to_string(i), &v).IsNotFound()) << i;
  }

  delete db;
  RemoveAll(name);
}

// compaction 之后重开，数据必须仍然完整（MANIFEST 的增删记录要能正确重放）。
TEST(DBTest, CompactionSurvivesReopen) {
  Options opt = TinyBufferOptions("compact5");
  opt.l0_compaction_trigger = 4;
  const std::string name = TempDbName("compact5");
  RemoveAll(name);

  const int n = 1000;
  {
    DB* db;
    ASSERT_TRUE(DB::Open(opt, name, &db).ok());
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
    }
    for (int i = 0; i < n; i += 10) {
      ASSERT_TRUE(db->Delete("k" + std::to_string(i)).ok());
    }
    delete db;
  }
  {
    DB* db;
    ASSERT_TRUE(DB::Open(opt, name, &db).ok());
    for (int i = 0; i < n; ++i) {
      std::string v;
      const std::string k = "k" + std::to_string(i);
      if (i % 10 == 0) {
        EXPECT_TRUE(db->Get(k, &v).IsNotFound()) << k;
      } else {
        ASSERT_TRUE(db->Get(k, &v).ok()) << k;
        EXPECT_EQ("v" + std::to_string(i), v) << k;
      }
    }
    delete db;
  }
  RemoveAll(name);
}

// 反复开关数据库：MANIFEST 不断追加增删记录，重开不得报错、数据不得丢。
TEST(DBTest, CompactionRepeatedReopen) {
  Options opt = TinyBufferOptions("compact6");
  opt.l0_compaction_trigger = 4;
  const std::string name = TempDbName("compact6");
  RemoveAll(name);

  const int rounds = 5;
  const int per_round = 200;
  for (int r = 0; r < rounds; ++r) {
    DB* db;
    ASSERT_TRUE(DB::Open(opt, name, &db).ok())
        << "reopen failed at round " << r;
    for (int i = 0; i < per_round; ++i) {
      const std::string k = "r" + std::to_string(r) + "_" + std::to_string(i);
      ASSERT_TRUE(db->Put(k, "v").ok()) << k;
    }
    // 验证历史轮次的数据仍在（跨重开、跨 compaction）
    for (int p = 0; p < r; ++p) {
      for (int i = 0; i < per_round; i += 7) {
        std::string v;
        ASSERT_TRUE(db->Get("r" + std::to_string(p) + "_" + std::to_string(i),
                            &v).ok())
            << "round " << p << " key " << i;
      }
    }
    delete db;
  }
  RemoveAll(name);
}
// 后台化的核心收益：压缩期间写入不被阻塞。
//
// 这个测试不追求精确的耗时数字（那会因机器而异），而是验证一个可判定的性质：
// compaction 正在大量进行时，写入依然能持续完成——若压缩还在写路径同步执行，
// 写请求会被整段压缩时间卡住。
TEST(DBTest, WritesProceedWhileCompactionRuns) {
  Options opt = TinyBufferOptions("bgwrite");
  opt.l0_compaction_trigger = 2;  // 频繁触发，保证压缩一直在跑
  opt.write_buffer_size = 512;
  const std::string name = TempDbName("bgwrite");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 先写够数据把压缩挑起来
  for (int i = 0; i < 400; ++i) {
    ASSERT_TRUE(db->Put("warm" + std::to_string(i), "v").ok());
  }

  // 一边继续写（持续触发 flush 与 compaction），一边读，全程必须能完成。
  const int n = 600;
  for (int i = 0; i < n; ++i) {
    const std::string k = "live" + std::to_string(i);
    ASSERT_TRUE(db->Put(k, "v").ok()) << i;
    if (i % 50 == 0) {
      std::string v;
      ASSERT_TRUE(db->Get(k, &v).ok()) << i;
      EXPECT_EQ("v", v);
    }
  }
  for (int i = 0; i < n; ++i) {
    std::string v;
    ASSERT_TRUE(db->Get("live" + std::to_string(i), &v).ok()) << i;
  }

  // 迭代器在压缩进行中同样要可用（它会借用 Table 引用）
  {
  std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
  int seen = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++seen;
  EXPECT_TRUE(it->status().ok());
  EXPECT_GT(seen, 0);
  }

  delete db;
  RemoveAll(name);
}

// 并发读写 + 后台压缩：数据必须始终正确，压缩不得让任何写入丢失。
TEST(DBTest, ConcurrentWritesWithBackgroundCompaction) {
  Options opt = TinyBufferOptions("bgconcurrent");
  opt.l0_compaction_trigger = 2;
  opt.write_buffer_size = 1024;
  const std::string name = TempDbName("bgconcurrent");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  const int nthreads = 6;
  const int per_thread = 300;
  std::vector<std::thread> threads;
  for (int t = 0; t < nthreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < per_thread; ++i) {
        const std::string k = "t" + std::to_string(t) + "_" + std::to_string(i);
        const std::string v = "v" + std::to_string(i);
        EXPECT_TRUE(db->Put(k, v).ok());
        std::string got;
        EXPECT_TRUE(db->Get(k, &got).ok()) << k;
        EXPECT_EQ(v, got) << k;
      }
    });
  }
  for (auto& th : threads) th.join();

  for (int t = 0; t < nthreads; ++t) {
    for (int i = 0; i < per_thread; ++i) {
      std::string got;
      ASSERT_TRUE(db->Get("t" + std::to_string(t) + "_" + std::to_string(i),
                          &got).ok());
    }
  }

  delete db;
  RemoveAll(name);
}

// ===========================================================================
// W8 / P1：反向遍历、TableCache LRU、Snapshot 句柄、Compaction 优先级与限流
// ===========================================================================

// 反向遍历必须与正向完全对称：把正向收集到的 key 序列倒过来，应当正好是
// 反向收集到的序列。单独验证"反向前几个"很容易漏掉跨块、多版本的问题。
TEST(DBTest, ReverseIterationMirrorsForward) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("rev_mirror");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 数量刻意超过单块容量，迫使数据跨多个 data block —— 跨块边界是反向遍历
  // 最容易出错的地方（index 迭代器与 data 块迭代器必须同步后退）。
  const int N = 500;
  for (int i = 0; i < N; ++i) {
    const std::string k = "key" + std::string(4, '0') + std::to_string(i);
    ASSERT_TRUE(db->Put(k, "v" + std::to_string(i)).ok());
  }

  std::vector<std::string> forward;
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      forward.push_back(it->key().ToString());
    }
  }
  ASSERT_EQ(forward.size(), static_cast<size_t>(N));

  std::vector<std::string> backward;
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    for (it->SeekToLast(); it->Valid(); it->Prev()) {
      backward.push_back(it->key().ToString());
    }
  }
  ASSERT_EQ(backward.size(), forward.size());
  for (size_t i = 0; i < forward.size(); ++i) {
    EXPECT_EQ(backward[i], forward[forward.size() - 1 - i])
        << "第 " << i << " 个反向 key 与正向镜像不符";
  }

  delete db;
  RemoveAll(name);
}

// 反向遍历必须与正向给出**相同的 value**，而不只是相同的 key：
// MVCC 下同 user_key 有多个版本，反向若取错版本，key 序列照样正确。
TEST(DBTest, ReverseIterationSeesSameVersions) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("rev_mvcc");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int round = 1; round <= 4; ++round) {
    for (int i = 0; i < 100; ++i) {
      const std::string k = "k" + std::string(3, '0') + std::to_string(i);
      // 每轮把值改成"轮次-键"，这样任何读到旧版本的行为都能被值暴露出来
      ASSERT_TRUE(db->Put(k, "r" + std::to_string(round) + "-" +
                                 std::to_string(i)).ok());
    }
  }
  ASSERT_TRUE(db->Delete("k050").ok());  // 墓碑不得出现在任一方向

  std::vector<std::pair<std::string, std::string>> forward, backward;
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      forward.emplace_back(it->key().ToString(), it->value().ToString());
    }
  }
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    for (it->SeekToLast(); it->Valid(); it->Prev()) {
      backward.emplace_back(it->key().ToString(), it->value().ToString());
    }
  }

  ASSERT_FALSE(forward.empty());
  ASSERT_EQ(forward.size(), backward.size());
  for (size_t i = 0; i < forward.size(); ++i) {
    const auto& f = forward[forward.size() - 1 - i];
    EXPECT_EQ(backward[i].first, f.first);
    EXPECT_EQ(backward[i].second, f.second) << "反向读到了非最新版本";
  }
  // 墓碑在两个方向都不应出现
  for (const auto& kv : forward) EXPECT_NE(kv.first, "k050");

  delete db;
  RemoveAll(name);
}

// Prev 退到第一个之后 Valid() 必须为 false（而不是绕回末尾或崩）
TEST(DBTest, ReverseIterationStopsAtStart) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("rev_stop");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("b", "2").ok());

  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    it->SeekToLast();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ("b", it->key().ToString());
    it->Prev();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ("a", it->key().ToString());
    it->Prev();
    EXPECT_FALSE(it->Valid()) << "退过第一个之后应报告无效";
  }

  delete db;
  RemoveAll(name);
}

// TableCache 必须真的按容量上限回收，否则长时间运行内存无上限。
TEST(DBTest, TableCacheRespectsCapacityLimit) {
  Options opt;
  opt.create_if_missing = true;
  // 极小的缓存上限：只能装下 0 个文件，任何一次 Acquire 都应触发淘汰。
  // 这让断言不依赖"文件大小到底是多少"这种脆弱前提。
  opt.max_table_cache_bytes = 1;
  // 必须真的产生 SSTable：否则数据全在 MemTable，Get 不碰文件，缓存条目恒为 0，
  // 断言会因"0 <= 1"而空过 —— 测试看着绿，实际什么都没验证到。
  opt.write_buffer_size = 512;
  const std::string name = TempDbName("cache_lru");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i), "v" + std::to_string(i)).ok());
  }

  // 全部读一遍，确保每个文件都被打开过
  for (int i = 0; i < 500; ++i) {
    std::string got;
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &got).ok())
        << "淘汰后重开文件必须仍能正确读到 key" << i;
  }

  ASSERT_GT(db->NumTableFiles(), 1u) << "本测试前提：应已产生多个 SSTable";
  EXPECT_LE(db->TableCacheEntries(), 1u)
      << "缓存条目数应被容量上限压住（当前 " << db->TableCacheEntries() << "）";
  EXPECT_LE(db->TableCacheBytes(), 1u)
      << "缓存字节数应被容量上限压住（当前 " << db->TableCacheBytes() << "）";

  delete db;
  RemoveAll(name);
}

// 上限设得足够大时不应发生淘汰：这是上一个测试的对照组，
// 否则"淘汰逻辑把缓存清空了"也会让上一个测试通过。
TEST(DBTest, TableCacheKeepsEntriesUnderGenerousLimit) {
  Options opt;
  opt.create_if_missing = true;
  opt.max_table_cache_bytes = 256u << 20;  // 256MB，远超本测试产生的数据量
  // 必须真的产生 SSTable 才有缓存条目：默认 4MB 缓冲下 200 条小写入全留在
  // MemTable 里，Get 根本不碰文件，缓存会是空的，断言就失去意义。
  opt.write_buffer_size = 512;
  const std::string name = TempDbName("cache_big");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i), "v" + std::to_string(i)).ok());
  }
  for (int i = 0; i < 200; ++i) {
    std::string got;
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &got).ok());
  }

  ASSERT_GT(db->NumTableFiles(), 0u) << "本测试前提：应已产生 SSTable";
  EXPECT_GT(db->TableCacheEntries(), 0u) << "上限宽松时不应淘汰任何条目";
  EXPECT_EQ(db->TableCacheEntries(), db->NumTableFiles())
      << "每个被读过的文件都应留在缓存里";

  delete db;
  RemoveAll(name);
}

// Snapshot 句柄 + 点查历史读
TEST(DBTest, SnapshotHandleReadsHistoricalValue) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("snap_handle");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_TRUE(db->Put("k", "v1").ok());

  const Snapshot* snap = db->GetSnapshot();
  ASSERT_NE(snap, nullptr);

  ASSERT_TRUE(db->Put("k", "v2").ok());
  ASSERT_TRUE(db->Put("k", "v3").ok());

  std::string got;
  ASSERT_TRUE(db->Get("k", &got).ok());
  EXPECT_EQ("v3", got) << "默认读应看到最新值";

  // 快照读：点查与迭代都必须回到快照时刻的视图
  ReadOptions ro(snap);
  ASSERT_TRUE(db->Get("k", &got, ro).ok());
  EXPECT_EQ("v1", got) << "快照点查应读到取快照时的值";

  {
    std::unique_ptr<Iterator> it(db->NewIterator(ro));
    it->SeekToFirst();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ("v1", it->value().ToString());
    it->Next();
    EXPECT_FALSE(it->Valid());
  }

  // 释放快照不会改变已经构造好的 ro —— 它持有的 sequence 仍是取快照时的值，
  // 因此继续用它读**依然**是 v1。这是设计使然：快照点由句柄创建时确定，
  // 释放只向数据库注销"还有人在读这个视图"，不追溯修改调用方手里的参数。
  db->ReleaseSnapshot(snap);
  ASSERT_TRUE(db->Get("k", &got, ro).ok());
  EXPECT_EQ("v1", got) << "已释放的句柄对应的历史读仍应稳定复现同一视图";

  // 用默认选项读，才是"当前最新"
  ASSERT_TRUE(db->Get("k", &got, ReadOptions()).ok());
  EXPECT_EQ("v3", got);

  delete db;
  RemoveAll(name);
}

// Snapshot 的真正价值：跨 compaction 保持视图。
// 若压缩把快照依赖的旧版本丢掉，这里读到的就会是新值或 NotFound。
TEST(DBTest, SnapshotSurvivesCompaction) {
  Options opt;
  opt.create_if_missing = true;
  // 小缓冲 + 低阈值，制造大量 L0 文件，逼出 compaction
  opt.write_buffer_size = 512;
  opt.l0_compaction_trigger = 2;
  const std::string name = TempDbName("snap_compact");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 先把 key 推到磁盘（flush），再取快照 —— 这样快照依赖的版本在 SSTable 里，
  // 会被后续 compaction 反复处理，正是要验证的场景。
  ASSERT_TRUE(db->Put("key", "gen1").ok());
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(db->Put("filler" + std::to_string(i), "x" + std::to_string(i)).ok());
  }

  const Snapshot* snap = db->GetSnapshot();
  ASSERT_NE(snap, nullptr);
  ReadOptions ro(snap);

  // 大量写入 + 覆盖，触发多轮 flush 与 compaction
  for (int i = 0; i < 400; ++i) {
    ASSERT_TRUE(db->Put("key", "gen2").ok());
    ASSERT_TRUE(db->Put("filler" + std::to_string(i), "y" + std::to_string(i)).ok());
  }

  // 强制让后台压缩有机会跑完（写入本身会触发，析构也会等它退出）
  std::string got;
  ASSERT_TRUE(db->Get("key", &got, ro).ok());
  EXPECT_EQ("gen1", got) << "compaction 之后快照视图必须保持不变";
  ASSERT_TRUE(db->Get("key", &got).ok());
  EXPECT_EQ("gen2", got) << "普通读应看到最新值";

  db->ReleaseSnapshot(snap);
  delete db;
  RemoveAll(name);
}

// 多层同时超限时应压缩压力最大的层，而不是永远按固定顺序先压 L0。
TEST(DBTest, CompactionPicksHighestPressureLevel) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 512;
  opt.l0_compaction_trigger = 4;
  opt.max_level_bytes = 1024;           // 小层容量，便于把深层顶过阈值
  opt.max_level_bytes_multiplier = 2;  // 逐层翻倍：L1=1KB L2=2KB L3=4KB
  const std::string name = TempDbName("priority");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 写入足够多的数据，把多层都顶过容量
  for (int i = 0; i < 4000; ++i) {
    ASSERT_TRUE(db->Put("key" + std::string(6, '0') + std::to_string(i),
                        "value" + std::to_string(i) + std::string(32, 'x')).ok());
  }

  // 数据正确性不能因为压缩策略变化而受损
  for (int i = 0; i < 4000; ++i) {
    std::string got;
    ASSERT_TRUE(db->Get("key" + std::string(6, '0') + std::to_string(i), &got).ok())
        << "压缩后仍应读得到第 " << i << " 个键";
    EXPECT_EQ("value" + std::to_string(i) + std::string(32, 'x'), got);
  }

  // 每层文件数应受容量约束（不允许某一层无限膨胀）
  const auto counts = db->GetLevelFileCounts();
  ASSERT_FALSE(counts.empty());
  EXPECT_LE(counts[0], 32u) << "L0 文件数应被限制住";

  delete db;
  RemoveAll(name);
}

// 限流不能把压缩彻底饿死：即使写入量很小，也必须能把积压清掉。
TEST(DBTest, ThrottlingNeverStarvesCompaction) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 512;
  opt.l0_compaction_trigger = 2;
  // 把限流调到极紧：压缩写出的字节数几乎不允许超过写入量。
  opt.compaction_max_write_amplification = 1;
  opt.compaction_backlog_factor = 2;
  const std::string name = TempDbName("throttle");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  for (int i = 0; i < 600; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i), "v" + std::to_string(i)).ok());
  }

  // 逃生阀的作用：即便限流很紧，L0 也不该无限堆积
  EXPECT_LE(db->GetLevelFileCounts()[0], 16u)
      << "积压上限未生效，限流把压缩饿死了";

  // 数据仍然完整
  for (int i = 0; i < 600; ++i) {
    std::string got;
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &got).ok())
        << "限流不应导致数据丢失";
  }

  delete db;
  RemoveAll(name);
}

// ===========================================================================
// 审查发现的缺陷回归测试
//
// 这些用例不是为了"让实现显得被测过"，而是审查报告里点名的两处 UAF 与一处
// 静默损坏**长期未被发现的根因**：迭代测试与并发测试分开跑，没有交叉覆盖。
// ===========================================================================

// 迭代器存活期间，另一线程持续写入触发 flush + compaction。
//
// 【这条用例专门覆盖两处 use-after-free 的触发条件】
//   * CompactLevel 把 Version 里 FileMetaData 元素的裸指针存进 vector 后就 Unref，
//     随后并发 LogAndApply（后台 compaction 与写路径 flush 都会换版本）会让旧
//     Version 被 delete —— 后续解引用即 UAF。
//   * NewIterator 若只在锁内 load 而把 Ref 放到锁外，flush 换表时旧 MemTable 可能
//     被 delete，适配器随后对它 Ref 即 UAF。
//
// 两者都不会被 ASAN/TSAN 在"迭代测试"或"并发测试"里抓到，因为缺少这个交叉。
// 本例不试图断言某个具体竞态一定发生，而是把并发窗口开到足够宽：任何一次
// UAF 都会让 ASAN 直接失败，这是"验证会发生什么"而非"验证不发生什么"。
TEST(DBTest, IteratorSurvivesConcurrentFlushAndCompaction) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 1024;      // 频繁 flush
  opt.l0_compaction_trigger = 2;    // 频繁 compaction
  opt.max_level_bytes = 4096;       // 更容易触达多层，制造换版本
  opt.max_level_bytes_multiplier = 2;
  const std::string name = TempDbName("iter_concurrent");
  RemoveAll(name);

  DB* db;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());

  // 基线数据，保证迭代器拿到的是一个非空快照
  const int kBase = 300;
  for (int i = 0; i < kBase; ++i) {
    ASSERT_TRUE(db->Put("key" + std::string(4, '0') + std::to_string(i),
                        "base" + std::to_string(i)).ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> written{0};
  std::thread writer([&] {
    int i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (db->Put("new" + std::to_string(i),
                  "payload" + std::string(64, 'y')).ok()) {
        written.fetch_add(1, std::memory_order_relaxed);
      }
      ++i;
    }
  });

  // 反复遍历：每次 SeekToFirst 都要求 VersionSet / MemTable 重新定位，
  // 正好穿过 load 与 Ref 之间、Unref 与解引用之间的那些窗口。
  int last_seen = 0;
  for (int round = 0; round < 60 && written.load(std::memory_order_relaxed) < 200;
       ++round) {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    it->SeekToFirst();
    int seen = 0;
    while (it->Valid()) {
      ++seen;
      it->Next();
    }
    ASSERT_TRUE(it->status().ok())
        << "并发写入期间迭代器状态异常（round=" << round << "）";
    // 迭代器是快照读：至少应看到创建时的全部基线数据（并发写入的看不到）
    EXPECT_GE(seen, kBase) << "迭代器丢了基线数据（round=" << round << "）";
    last_seen = seen;
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();
  EXPECT_GT(written.load(std::memory_order_relaxed), 0) << "并发写入线程未跑起来";
  EXPECT_GT(last_seen, 0);

  delete db;
  RemoveAll(name);
}

// MANIFEST **中段**损坏必须让 Open 失败，绝不能被当成"正常读完"。
//
// 【这条用例验证的是"绝不静默丢数据"原则的最后一道防线】
// 若损坏被静默接受，重放会提前终止并被判为成功，于是：
//   log_number 退回到损坏点之前的值 → 指向新 WAL 的记录被丢弃
//   → 恢复逻辑把那个新 WAL 当孤儿删除；同时本次 flush 已登记的 SSTable 也成孤儿
//   → 数据在两个方向上同时消失，全程没有任何错误返回。
//
// 尾部截断是**另一回事**（写到一半崩溃，fsync 边界，正常丢弃即可），
// 由 Reader 归为正常 EOF。这条用例只针对中段损坏。
TEST(DBTest, CorruptedManifestMiddleIsRejected) {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 512;    // 多次 flush => MANIFEST 里有多条记录
  const std::string name = TempDbName("manifest_corrupt");
  RemoveAll(name);

  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(opt, name, &db).ok());
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Put("key" + std::to_string(i), "v" + std::to_string(i)).ok());
    }
    delete db;  // 正常关闭，确保 MANIFEST 有足够多的记录
  }

  const std::string manifest = name + "/MANIFEST";
  uint64_t fsize = 0;
  ASSERT_TRUE(Env::Default()->GetFileSize(manifest, &fsize).ok());
  ASSERT_GT(fsize, 100u) << "MANIFEST 过小，无法构造中段损坏";

  {
    // 从文件中部开始翻转 64 个字节。MANIFEST 的记录是连续排列的（只有块边界
    // 才有填充），这个长度必然命中至少一条记录的 header 或 payload，
    // 使其 CRC 校验失败 —— 这正是"中段损坏"的形态。
    std::fstream f(manifest, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "无法以读写方式打开 MANIFEST";
    f.seekg(static_cast<std::streamoff>(fsize / 2));
    for (int i = 0; i < 64; ++i) {
      const std::streampos pos = f.tellg();
      if (pos < 0) break;
      f.seekg(pos);
      char c = 0;
      f.read(&c, 1);
      if (!f) break;
      const char flipped = static_cast<char>(c ^ 0xFF);
      f.seekp(pos);
      f.write(&flipped, 1);
    }
    f.flush();
  }

  DB* db = nullptr;
  const Status s = DB::Open(opt, name, &db);
  EXPECT_FALSE(s.ok()) << "MANIFEST 中段损坏必须让 Open 失败，而不是静默接受";
  EXPECT_TRUE(s.IsCorruption()) << "应报告为 Corruption，实际：" << s.ToString();
  EXPECT_EQ(db, nullptr);
  RemoveAll(name);
}

// create_if_missing 的默认语义此前从未被实现：默认 Options{} 打开不存在的库
// 会静默创建，与选项含义完全相反。
TEST(DBTest, CreateIfMissingFalseRejectsMissingDirectory) {
  Options opt;  // create_if_missing 默认 false
  const std::string name = TempDbName("no_create");
  RemoveAll(name);

  DB* db = nullptr;
  const Status s = DB::Open(opt, name, &db);
  EXPECT_TRUE(s.IsInvalidArgument())
      << "默认配置下打开不存在的库应报错，实际：" << s.ToString();
  EXPECT_EQ(db, nullptr);
  RemoveAll(name);
}

TEST(DBTest, ErrorIfExistsRejectsExistingDirectory) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = TempDbName("exists");
  RemoveAll(name);

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(opt, name, &db).ok());
  ASSERT_TRUE(db->Put("k", "v").ok());
  delete db;

  Options opt2;
  opt2.create_if_missing = true;
  opt2.error_if_exists = true;
  DB* db2 = nullptr;
  const Status s = DB::Open(opt2, name, &db2);
  EXPECT_TRUE(s.IsInvalidArgument()) << "error_if_exists 应拒绝已存在的目录";
  EXPECT_EQ(db2, nullptr);
  RemoveAll(name);
}

// 非法配置必须在 Open 时就被挡下，而不是留到某次压缩时以整数除零的形式爆出来。
TEST(DBTest, InvalidOptionsAreRejectedAtOpen) {
  const std::string name = TempDbName("bad_opts");
  RemoveAll(name);

  {
    Options opt;
    opt.create_if_missing = true;
    opt.max_level_bytes_multiplier = 0;  // 会让 LevelCapacity 里 UINT64_MAX/0
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(opt, name, &db).IsInvalidArgument())
        << "multiplier=0 应在 Open 被拒";
    EXPECT_EQ(db, nullptr);
  }
  {
    Options opt;
    opt.create_if_missing = true;
    opt.max_num_levels = 1;  // 没有 L1，压缩无处可去
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(opt, name, &db).IsInvalidArgument())
        << "max_num_levels=1 应在 Open 被拒";
    EXPECT_EQ(db, nullptr);
  }
  {
    Options opt;
    opt.create_if_missing = true;
    opt.write_buffer_size = 0;
    DB* db = nullptr;
    EXPECT_TRUE(DB::Open(opt, name, &db).IsInvalidArgument())
        << "write_buffer_size=0 应在 Open 被拒";
    EXPECT_EQ(db, nullptr);
  }
  RemoveAll(name);
}
}  // namespace
}  // namespace tinystore
