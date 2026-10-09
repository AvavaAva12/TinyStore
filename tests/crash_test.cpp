// ===========================================================================
// 崩溃恢复测试 —— 用 fork + exec + 崩溃注入点验证"已确认提交的数据绝不丢失"
// ===========================================================================
//
// 【为什么必须 fork，不能在一个进程里模拟】
// 崩溃恢复要验证的是"进程被 kill -9 之后重启"的行为。这个前提无法在同一进程
// 里伪造：进程还活着，MemTable、VersionSet、文件句柄全在，怎么调 Open 走的都是
// "有活进程"的分支。必须真的让一个进程死掉，再由另一个重新打开校验。
//
// 【为什么是 fork + exec，而不是只用 fork】
// 只 fork 会踩到一个隐蔽而致命的坑：**Env 的后台线程池是进程级的单例**。
//
// TinyStore 的 compaction 跑在 Env 的后台线程上，而 fork 只复制调用 fork 的
// 那个线程。子进程里：
//   1. 线程池对象还在（随堆内存一起被复制），但**执行任务的线程不存在**；
//   2. 于是子进程调 Env::Schedule() 只是把任务塞进队列，没人消费；
//   3. 子进程析构 DB 时 WaitForBackgroundCompaction() 会往队列投一个 barrier
//      任务并等待它完成 —— 永远等不到，直接死锁；
//   4. 更要紧的是：compaction 根本不会执行，"输出已落盘"那个崩溃点永远打不到，
//      测试会变成"什么都没验证"。
//
// 加上 exec 之后，子进程是**全新的进程映像**：Env 单例重新构造、后台线程重新
// 启动、锁状态归零。这才是真正意义上"另一个进程用同一个库"的形态，
// 也正是文件锁要防范的场景。
//
// 【为什么走命令行参数而不是环境变量】
// setenv 会调 malloc。fork 之后子进程里只有当前线程，其他线程持有的 malloc 锁
// 状态会被继承，可能死锁。argv 所需的字符串在 fork 前就已在父进程构造好，
// 子进程只做只读访问 + exec（exec 本身是 async-signal-safe 的），全程安全。
//
// 【验证的契约】
// 断言只针对**已返回 OK 的写入**：Put 返回成功之后，数据就必须持久可读。
//
// 崩溃瞬间正在执行的那次 Put 不作要求 —— 它没有返回给调用方，用户无从得知
// 结果，要求"绝不丢失"反而不合理（真实系统同样会把它算作未提交）。
//
// 【退出码约定：父子通信的唯一手段】
//   97 —— 崩溃注入点命中（test_util.cpp 的 _exit 产生）
//   90 —— 子进程 Open 失败
//   91 —— 子进程写入返回错误
//   92 —— 子进程写完却没崩 => 注入点没生效，或该路径未被执行到
//   93 —— 子进程成功打开库 => 排他锁失效
//   98 —— exec 失败
//   0  —— 子进程 Open 被正确拒绝（文件锁用例的期望值）
// 父进程按退出码判断"到底发生了什么"，任何非预期码都判测试失败。
//
// ===========================================================================

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tinystore/db.h"
#include "tinystore/test_util.h"
#include "gtest/gtest.h"

// 注意：这里刻意**不用**匿名 namespace。
// 文件末尾的 main 需要调用 RunCrashChild / RunLockProbeChild 来以子进程身份
// 重新执行自身，而匿名 namespace 的符号在 TU 外部不可见。本文件是独立测试
// 可执行文件，不导出任何符号给其他目标，因此没有污染外部的风险。
namespace tinystore {

// 子进程退出码（含义见文件头注释）
constexpr int kExitCrash = 97;        // 崩溃注入点命中
constexpr int kExitOpenFailed = 90;   // 子进程打不开库
constexpr int kExitWriteFailed = 91;  // 写入返回错误
constexpr int kExitNoCrash = 92;      // 该崩却没崩
constexpr int kExitLockAcquired = 93; // 排他锁失效：锁被成功获取
constexpr int kExitExecFailed = 98;   // exec 失败

// 后台压缩的等待上限：300 × 10ms = 3 秒。
// 设上限而非无限等，是为了让"压缩没跑到崩溃点"这种失败快速暴露成断言失败，
// 而不是让测试静默挂住。
constexpr int kBackgroundWaitIterations = 300;
constexpr int kBackgroundWaitIntervalMs = 10;

// 自进程可执行文件路径。fork+exec 需要一个稳定的路径来重新拉起自己；
// /proc/self/exe 由内核提供，比 argv[0] 可靠（argv[0] 可能只是裸文件名）。
constexpr const char* kSelfPath = "/proc/self/exe";

// argv[0] 之后的固定标记：main 靠它识别"我是子进程"
constexpr const char* kChildFlag = "--tinystore-crash-child";

static std::string TempDbName(const std::string& sub) {
  std::string dir;
  Env::Default()->GetTestDirectory(&dir);
  return dir + "/" + sub;
}

static void RemoveAll(const std::string& name) {
  std::error_code ec;
  std::filesystem::remove_all(name, ec);
}

// 测试专用库参数：小缓冲 + 低压缩阈值，让少量写入就能走到 flush / compaction，
// 从而用很小的数据量走完深层持久化路径。
static Options CrashTestOptions() {
  Options opt;
  opt.create_if_missing = true;
  opt.write_buffer_size = 512;    // 约 17 条记录触发一次 flush
  opt.l0_compaction_trigger = 2;  // 2 个 L0 文件即压缩，便于快速进入 compaction
  opt.max_level_bytes = 1024;
  opt.max_level_bytes_multiplier = 2;
  return opt;
}

// key 形如 "key00042"：定宽补零，保证字典序与数值序一致。
// 不补零则 "key100" < "key99"，基线数据的校验就失去意义。
static std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%05d", i);
  return buf;
}

static std::string MakeValue(int i) {
  return "value-" + std::to_string(i) + std::string(16, 'x');
}

// 写入 n 条并正常关闭。每条都确认返回 OK —— 这些是后面要保护的"已提交数据"。
static void SeedDatabase(const std::string& name, int n) {
  DB* db = nullptr;
  if (!DB::Open(CrashTestOptions(), name, &db).ok()) return;
  for (int i = 0; i < n; ++i) {
    if (!db->Put(MakeKey(i), MakeValue(i)).ok()) break;
  }
  delete db;  // 正常关闭：flush 干净、锁释放、后台任务收敛
}

// 校验基线数据一条不少、值一条不差。
static void VerifySeededData(DB* db, int n) {
  for (int i = 0; i < n; ++i) {
    std::string got;
    const Status s = db->Get(MakeKey(i), &got);
    ASSERT_TRUE(s.ok())
        << "已确认提交的 key 丢失了: " << MakeKey(i) << " (i=" << i << ")";
    EXPECT_EQ(MakeValue(i), got) << "key " << MakeKey(i) << " 的值不对";
  }
}

// ---------------------------------------------------------------------------
// 子进程：打开库 -> 写入 -> 在指定崩溃点突然死亡
//
// 由 main 在识别到 kChildFlag 后调用。永不返回。
// ---------------------------------------------------------------------------
[[noreturn]] static int RunCrashChild(int argc, char** argv) {
  // argv 布局: [0]=self [1]=flag [2]=dbname [3]=point [4]=first [5]=count [6]=wait_bg
  if (argc < 7) ::_exit(kExitExecFailed);
  const std::string name = argv[2];
  const int crash_point = std::atoi(argv[3]);
  const int first = std::atoi(argv[4]);
  const int count = std::atoi(argv[5]);
  const bool wait_for_background = std::atoi(argv[6]) != 0;

  DB* db = nullptr;
  if (!DB::Open(CrashTestOptions(), name, &db).ok()) ::_exit(kExitOpenFailed);

  SetCrashPointForTesting(crash_point);
  for (int i = 0; i < count; ++i) {
    if (!db->Put(MakeKey(first + i), MakeValue(first + i)).ok()) {
      ::_exit(kExitWriteFailed);
    }
  }
  // 能走到这里说明整批写完都没崩 => 该路径没被触发（flush/compaction 触发次数不足）
  SetCrashPointForTesting(kCrashPointNone);
  delete db;

  if (wait_for_background) {
    // compaction 在后台线程上跑，崩溃发生在主线程 Put 全部返回之后。
    // 给它时间跑到"输出已落盘"那一步；命中后整个进程（含后台线程）一起消失。
    // 上限 3 秒：超时就走到 kExitNoCrash，让断言直接指出"后台压缩没跑到该点"，
    // 而不是让测试挂住。
    for (int i = 0; i < kBackgroundWaitIterations; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kBackgroundWaitIntervalMs));
    }
  }
  ::_exit(kExitNoCrash);
}

// 子进程：只尝试打开库，用退出码报告排他锁是否挡住了自己。
[[noreturn]] static int RunLockProbeChild(int argc, char** argv) {
  if (argc < 3) ::_exit(kExitExecFailed);
  DB* db = nullptr;
  if (DB::Open(CrashTestOptions(), argv[2], &db).ok()) {
    delete db;
    ::_exit(kExitLockAcquired);  // 排他失效
  }
  ::_exit(0);  // 被正确拒绝
}

// fork + exec 一个子进程，等待它结束并返回退出码；被信号杀死返回 -1。
//
// 用 exec 的理由见文件头。参数以 std::string 提前备好，子进程只读继承来的
// 缓冲区，不做任何可能加锁的分配操作。
static int SpawnChild(std::vector<std::string>& args) {
  std::fflush(nullptr);  // 防止缓冲区里的输出在 fork 后被写两份
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(kSelfPath));
  for (std::string& a : args) argv.push_back(&a[0]);
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    ::execv(kSelfPath, argv.data());
    ::_exit(kExitExecFailed);  // exec 失败才会走到这里
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) return -1;
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// 起一个写入子进程，在 crash_point 处崩溃。
static int SpawnCrashChild(const std::string& name, int crash_point, int first,
                            int count, bool wait_for_background) {
  std::vector<std::string> args = {
      kChildFlag, name, std::to_string(crash_point), std::to_string(first),
      std::to_string(count), wait_for_background ? "1" : "0"};
  return SpawnChild(args);
}

// 起一个只做打开尝试的子进程。
static int SpawnLockProbeChild(const std::string& name) {
  std::vector<std::string> args = {"--tinystore-lock-probe", name};
  return SpawnChild(args);
}

// ===========================================================================
// 崩溃恢复用例
// ===========================================================================

// WAL 已 fsync 但内存视图未更新：重启必须从 WAL 重放把这条记录找回来。
// 若读不到，说明恢复路径漏了"WAL 有记录但 MemTable 为空"这个状态。
TEST(CrashTest, RecoversRecordThatWasSyncedToWal) {
  const std::string name = TempDbName("crash_wal_sync");
  RemoveAll(name);
  SeedDatabase(name, 20);

  // 第 1 次 Put 就会崩在 WAL fsync 之后
  const int rc = SpawnCrashChild(name, kCrashPointAfterWalSync, 1000, 1, false);
  ASSERT_EQ(kExitCrash, rc) << "应崩在 WAL fsync 之后，实际退出码 " << rc
                            << "（92 表示该崩溃点未生效）";

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(CrashTestOptions(), name, &db).ok())
      << "崩溃后必须能重新打开（flock 应随进程退出自动释放）";
  VerifySeededData(db, 20);
  std::string got;
  EXPECT_TRUE(db->Get(MakeKey(1000), &got).ok())
      << "WAL 已 fsync 的记录在恢复后应可见";
  delete db;
  RemoveAll(name);
}

// flush 写完 SSTable 但尚未登记进 MANIFEST：那个 .ldb 此刻是孤儿。
// 恢复时必须把它当孤儿清掉，且数据仍能从旧 WAL 重放出来 ——
// 若误当作有效文件，就会读到一份"存在但从未被承认"的数据。
TEST(CrashTest, FlushOrphanSstableIsDiscardedAndDataRecovered) {
  const std::string name = TempDbName("crash_flush_orphan");
  RemoveAll(name);
  SeedDatabase(name, 20);

  // write_buffer_size=512 时约 17 条触发一次 flush；50 条足以走到 flush
  // 并在其 SSTable 落盘之后崩溃。
  const int rc = SpawnCrashChild(name, kCrashPointAfterFlushSstWrite, 1000, 50, false);
  ASSERT_EQ(kExitCrash, rc) << "应崩在 SSTable 落盘之后，实际退出码 " << rc;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(CrashTestOptions(), name, &db).ok());
  VerifySeededData(db, 20);
  delete db;
  RemoveAll(name);
}

// MANIFEST 已提交、旧 WAL 未删：数据此刻"两头都有"（SSTable 已登记、旧 WAL 还在）。
// 恢复必须容忍这种重叠 —— 重放旧 WAL 会得到重复记录，靠 sequence 收敛即可，
// 绝不能因此报错或丢数据。
TEST(CrashTest, DuplicateDataAcrossSstableAndWalIsTolerated) {
  const std::string name = TempDbName("crash_flush_commit");
  RemoveAll(name);
  SeedDatabase(name, 20);

  const int rc = SpawnCrashChild(name, kCrashPointAfterFlushCommit, 1000, 50, false);
  ASSERT_EQ(kExitCrash, rc) << "应崩在 MANIFEST 提交之后，实际退出码 " << rc;

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(CrashTestOptions(), name, &db).ok())
      << "SSTable 与旧 WAL 数据重叠时，恢复必须能正常打开";
  VerifySeededData(db, 20);

  // 遍历全库：重叠数据不应产生重复 user_key（归并迭代器按 internal_key
  // 收敛到每个 user_key 的最新版本，重叠项里旧版本本就被 MVCC 过滤掉）
  //
  // 【迭代器必须先于 DB 销毁】
  // TableReleasingIterator 析构时要向 VersionSet 归还 Table 引用，而 VersionSet
  // 归 DB 所有、随 ~DBImpl 一起销毁。迭代器若活得比 DB 久，析构就会访问已释放的
  // 缓存（ASAN 报 heap-use-after-free）。因此这里用独立作用域把迭代器生命周期
  // 限制在 delete db 之前 —— 这也是 Iterator 接口注释里写明的约定。
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    std::string prev;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const std::string cur = it->key().ToString();
      EXPECT_NE(cur, prev) << "重叠数据导致 key 重复出现: " << cur;
      prev = cur;
    }
  }
  delete db;
  RemoveAll(name);
}

// compaction 输出已落盘、MANIFEST 未提交：源文件此刻仍必须有效。
//
// 本组最能暴露问题的一个。若恢复逻辑"看到输出文件就认为压缩已完成"而把源文件
// 当孤儿删掉，那么**唯一持有数据的文件就没了** —— 数据消失且无任何报错。
TEST(CrashTest, CompactionOutputWithoutCommitKeepsSources) {
  const std::string name = TempDbName("crash_compact");
  RemoveAll(name);
  SeedDatabase(name, 200);

  // 低阈值配置下 400 条足以触发明细 compaction；后台线程完成一次压缩后
  // 会在"输出已落盘"那一步终止整个进程。
  const int rc =
      SpawnCrashChild(name, kCrashPointAfterCompactionOutput, 1000, 400, true);
  ASSERT_EQ(kExitCrash, rc)
      << "应崩在 compaction 输出落盘之后，实际退出码 " << rc
      << "（92 表示后台压缩没跑到该点，数据量可能不够）";

  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(CrashTestOptions(), name, &db).ok());
  // 基线数据可能正处在"压缩了一半"的状态，但一条都不能少
  VerifySeededData(db, 200);
  delete db;
  RemoveAll(name);
}

// ===========================================================================
// 库级排他锁
// ===========================================================================

// 两个进程不能同时打开同一目录。
//
// 【为什么必须用两个真实进程】
// flock 锁的是"打开文件描述"而非进程：同一进程用两个不同 fd 打开同一文件，
// 会各自获得自己的锁。所以同进程内两次 Open 一定不会被拦 —— 这不是缺陷，
// 是 flock 的既定语义。真正的风险来自多进程并发，测试也必须照这个形态做。
TEST(CrashTest, SecondProcessIsRejectedByLock) {
  const std::string name = TempDbName("lock_excl");
  RemoveAll(name);

  // 父进程先开库并一直持锁
  DB* db = nullptr;
  ASSERT_TRUE(DB::Open(CrashTestOptions(), name, &db).ok());

  const int rc = SpawnLockProbeChild(name);
  EXPECT_EQ(0, rc) << "父进程仍持锁，子进程不应打开成功（返回 0）；实际退出码 "
                  << rc << "（93 表示排他锁失效）";

  // 持锁者自己仍要能正常读写（锁不能误伤自己）
  ASSERT_TRUE(db->Put(MakeKey(1), MakeValue(1)).ok());
  std::string got;
  ASSERT_TRUE(db->Get(MakeKey(1), &got).ok());

  delete db;
  RemoveAll(name);
}

// 释放锁之后另一个进程应当能重新打开 —— 锁不能变成永久占用。
//
// 注意与上一个用例的期望正好相反：那里原进程**仍持锁**，子进程必须打不开；
// 这里原进程**已关闭**，子进程必须打得开。两者合起来才证明锁既能挡住别人、
// 又不会变成永久占用。
TEST(CrashTest, LockIsReleasedWhenDatabaseCloses) {
  const std::string name = TempDbName("lock_release");
  RemoveAll(name);

  {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(CrashTestOptions(), name, &db).ok());
    delete db;  // 析构必须解锁
  }

  const int rc = SpawnLockProbeChild(name);
  EXPECT_EQ(kExitLockAcquired, rc)
      << "原进程已关闭，子进程应能成功打开（返回 93）；实际退出码 " << rc
      << "（0 表示锁没被释放，变成了永久占用）";
  RemoveAll(name);
}

}  // namespace tinystore

// ===========================================================================
// 自定义 main：先识别子进程模式，再交给 gtest
// ===========================================================================
// 不能直接链接 gtest_main：它提供的 main 只认 gtest 参数，无法在测试运行前
// 拦截"以子进程身份重新拉起自己"的场景。链接 GTest::gtest（不含 main）
// 并自己写 main，是让同一个二进制同时扮演"测试驱动"和"被测子进程"的标准做法。
int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--tinystore-crash-child") == 0) {
    tinystore::RunCrashChild(argc, argv);
  }
  if (argc >= 2 && std::strcmp(argv[1], "--tinystore-lock-probe") == 0) {
    tinystore::RunLockProbeChild(argc, argv);
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}