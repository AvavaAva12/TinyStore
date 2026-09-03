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

}  // namespace
}  // namespace tinystore
