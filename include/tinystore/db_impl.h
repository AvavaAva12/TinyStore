#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include "tinystore/db.h"
#include "tinystore/env.h"
#include "tinystore/internal_key.h"
#include "tinystore/log_reader.h"
#include "tinystore/log_writer.h"
#include "tinystore/memtable.h"
#include "tinystore/write_batch.h"

namespace tinystore {

// ===========================================================================
// Group Commit 的写请求单元
// ===========================================================================
//
// 每个调用 Write 的线程在栈上创建一个 Writer，把自己的 batch 挂上去，
// 然后把指针塞进 DBImpl::writers_ 队列等待提交：
//   * 若自己是队首 -> 成为 leader，负责把整组合并、写 WAL、回放 MemTable，
//     最后挨个唤醒同组的 follower；
//   * 若自己不是队首 -> 有别的 leader 在干活，安静等 leader 唤醒即可
//     （leader 会把本请求的 batch 一起提交，结果填进 status）。
//
// 这样多个并发写入共享"一次 WAL 追加 + 一次 fsync"，把毫秒级的 fsync 开销
// 摊薄到整组，通常带来数量级的写入 QPS 提升（见 W3 设计笔记）。
struct Writer {
  const WriteBatch* batch = nullptr;  // 本请求要写的 batch（只读，leader 不改它）
  Status status;                     // leader 填好的提交结果（同组共享）
  bool done = false;                 // leader 是否已提交完本请求
  std::condition_variable cv;         // follower 在此等待 leader 唤醒
};

// ===========================================================================
// DBImpl —— DB 的具体实现
// ===========================================================================
class DBImpl : public DB {
public:
  DBImpl(const Options& options, const std::string& name);
  ~DBImpl() override;

  Status Put(const Slice& key, const Slice& value) override;
  Status Delete(const Slice& key) override;
  Status Get(const Slice& key, std::string* value) override;
  Status Write(const WriteBatch& batch) override;

  // 从 WAL 恢复 MemTable，并新建一条可写的 WAL。由 DB::Open 调用。
  Status Recover();

private:
  Env* env_;
  const Comparator* user_comparator_;
  InternalKeyComparator icmp_;
  std::string dbname_;
  std::string wal_name_;  // 当前 WAL 文件名（W3 单文件，W4 起会按序号滚动）

  // --- 写路径同步 ---
  // mutex_ 只用于：保护 writers_ 队列 + 把"WAL 追加 + fsync + 回放"串化成单写者。
  // 它**不**保护读路径——Get 通过 mem_ / last_sequence_ 的 atomic 无锁完成。
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Writer*> writers_;  // GUARDED_BY(mutex_)

  // --- 内存状态（读路径无锁访问）---
  // 当前活跃 MemTable。W3 不做 flush，整个生命周期稳定；W4 引入 flush 后，
  // 这里会换成"可原子切换"的指针（immutable MemTable 入队落盘）。
  std::atomic<MemTable*> mem_{nullptr};
  // 已提交的最大 sequence。写者用 release 在"内存已落好"之后发布，
  // 读者用 acquire 读到它时，保证能看到对应的 MemTable 内容（MVCC 快照读基础）。
  std::atomic<SequenceNumber> last_sequence_{0};

  // --- WAL ---
  log::Writer* log_ = nullptr;  // 不拥有 dest_
  WritableFile* logfile_ = nullptr;
};

}  // namespace tinystore
