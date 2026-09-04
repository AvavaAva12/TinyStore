#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include "tinystore/db.h"
#include "tinystore/internal_key.h"
#include "tinystore/log_reader.h"
#include "tinystore/log_writer.h"
#include "tinystore/memtable.h"
#include "tinystore/version_set.h"
#include "tinystore/write_batch.h"

namespace tinystore {

// ===========================================================================
// Group Commit 的写请求单元（与 W3 相同）
// ===========================================================================
struct Writer {
  const WriteBatch* batch = nullptr;  // 本请求要写的 batch（只读，leader 不改它）
  Status status;                     // leader 填好的提交结果（同组共享）
  bool done = false;                 // leader 是否已提交完本请求
  std::condition_variable cv;         // follower 在此等待 leader 唤醒
};

// ===========================================================================
// DBImpl —— DB 的具体实现（W4：接上持久化层）
// ===========================================================================
//
// W4 在 W3 的写路径之上，把 MemTable 周期性 flush 成不可变 SSTable：
//   * 写：Group Commit 落到 WAL（一次 fsync）+ 回放 MemTable（同 W3）；
//   * flush：MemTable 超过 write_buffer_size 时整体写成一个 SSTable，登记进
//     VersionSet（MANIFEST 落盘），并滚动到新的 WAL；
//   * 读：先查 MemTable，未命中再按"从新到旧"扫描 SSTable 文件，合并出最新可见版本；
//   * 恢复：Open 时重放 MANIFEST 重建 Version（SSTable 集合 + 当前 WAL 编号），
//     再重放当前 WAL 把未 flush 的数据填回 MemTable。
//
// 读路径全程无锁：memtable 与 Version 都用 RCU 风格的 TryRef 安全取引用，
// SSTable 读取走 pread（线程安全），与 W3 的"无锁快照读"一脉相承。
class DBImpl : public DB {
public:
  DBImpl(const Options& options, const std::string& name);
  ~DBImpl() override;

  Status Put(const Slice& key, const Slice& value) override;
  Status Delete(const Slice& key) override;
  Status Get(const Slice& key, std::string* value) override;
  Status Write(const WriteBatch& batch) override;

  // 从 MANIFEST 恢复 Version + 当前 WAL 编号，重放 WAL，建立可写 WAL。
  Status Recover();

private:
  // 把活跃 MemTable flush 成一个 SSTable（登记进版本），并滚动 WAL。
  // 必须在持有 mutex_ 的写者（leader）上下文中调用。
  Status CompactMemTable();

  Env* env_;
  const Comparator* user_comparator_;
  InternalKeyComparator icmp_;
  Options options_;  // 持久化层调参（filter_policy / block_size / write_buffer_size）
  std::string dbname_;

  // --- 写路径同步（同 W3）---
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Writer*> writers_;

  // --- 内存状态（读路径无锁访问）---
  std::atomic<MemTable*> mem_{nullptr};  // 活跃 MemTable；flush 时 RCU 换出
  std::atomic<SequenceNumber> last_sequence_{0};

  // --- WAL ---
  WritableFile* logfile_ = nullptr;  // 活跃 WAL（不拥有，析构时关闭）
  log::Writer* log_ = nullptr;
  uint64_t logfile_number_ = 0;
  std::string wal_name_;

  // --- 版本 / MANIFEST ---
  VersionSet* versions_ = nullptr;  // 拥有
};

}  // namespace tinystore
