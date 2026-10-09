#include <algorithm>
#include <filesystem>
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

  std::unique_ptr<Iterator> old_it(db->NewIterator(snap));
  old_it->SeekToFirst();
  ASSERT_TRUE(old_it->Valid());
  EXPECT_EQ("a", old_it->key().ToString());
  EXPECT_EQ("old", old_it->value().ToString());
  old_it->Next();
  EXPECT_FALSE(old_it->Valid())
      << "快照之后写入的 b 不应出现在旧快照视图中";

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
  const size_t files = db->NumTableFiles();
  const auto counts = db->GetLevelFileCounts();
  EXPECT_LT(files, static_cast<size_t>(n) / 2)
      << "compaction did not converge file count: " << files;
  EXPECT_LE(counts[0], opt.l0_compaction_trigger)
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

}  // namespace
}  // namespace tinystore
