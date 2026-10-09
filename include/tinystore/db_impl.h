#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include "tinystore/db.h"
#include "tinystore/db_iterator.h"
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
// 读路径的并发模型：**取引用走极短临界区，数据访问全程无锁**。
//   * 取 MemTable / Version 的引用时，会短暂进入 mem_mutex_ / version_mutex_。
//     这两把锁只保护"指针获取 + 引用计数"，不包含任何数据访问，因此不构成
//     读写竞争。为什么这里必须加锁、以及为什么不能用"load 裸指针 + 失败重试"
//     的写法（那样是 use-after-free），见 VersionSet::current() 的注释。
//   * 临界区之外：MemTable 查找走跳表的无锁遍历，Version 遍历与 SSTable 读取
//     走 pread（天然线程安全），与 W3 的"无锁快照读"一脉相承。
class DBImpl : public DB {
public:
  DBImpl(const Options& options, const std::string& name);
  ~DBImpl() override;

  Status Put(const Slice& key, const Slice& value) override;
  Status Delete(const Slice& key) override;
  Status Get(const Slice& key, std::string* value) override;
  Status Write(const WriteBatch& batch) override;

  // 组装 DBIterator：MemTable + 全部 SSTable 作为归并的数据源。
  std::unique_ptr<Iterator> NewIterator(
      const ReadOptions& options) const override;

  // 从 MANIFEST 恢复 Version + 当前 WAL 编号，重放 WAL，建立可写 WAL。
  Status Recover();

private:
  // 把活跃 MemTable flush 成一个 SSTable（登记进版本），并滚动 WAL。
  // 必须在持有 mutex_ 的写者（leader）上下文中调用。
  Status CompactMemTable();

  // 把 level 层的文件与 level+1 层中键范围与之重叠的文件归并成新的
  // level+1 文件，并把源文件登记为删除。必须在持有 mutex_ 的写者（leader）
  // 上下文中调用。
  //
  // 这是 LSM 的核心收益来源：没有它，每次 flush 都新增一个 SSTable，文件数与
  // 读放大随写入量线性增长。
  //
  // 【墓碑为什么在这里被安全回收】
  // 归并用 DBIterator 驱动，而它只输出"当前可见的有效值"，墓碑不会被写出。
  // 这在 L0->L1 上是安全的：L1 之下已经没有更老的层，不存在该 key 的历史版本
  // 会因为墓碑消失而复活。但对 L1->L2 这类非最底层的压缩就不成立——那时
  // 必须先把墓碑原样带到下一层，等它沉到最后一层再丢。kFinalLevel 标记了
  // 当前的最底层，正是靠它区分这两种情形。
  Status CompactLevel(int level);

  // 若当前有层需要压缩，就往 Env 的后台线程池投一个任务。
  //
  // 【为什么 compaction 必须离开写路径】
  // W6 之前 compaction 是在写路径的 leader 里同步做的，期间整把 mutex_ 被占住，
  // 意味着**所有写请求都被阻塞**。而归并要做的是大量 SSTable 的读 + 写 +
  // fsync，属于 IO 密集型操作，耗时可达百毫秒级。把它放在写路径上，等于用
  // 一次压缩把整个库的写入停顿一下。
  //
  // 移到后台后，写路径只做"判断要不要压"（读几个计数器）然后立刻返回；
  // 真正的 IO 由后台线程承担。压缩期间写入照常进行，新写入可能又触发更多
  // flush，这正是我们想要的流水线。
  void MaybeScheduleCompaction();

  // 后台线程实际执行体：反复压缩直到所有层都回到阈值内。
  void BackgroundCompactionTask();

  // DB 析构前调用：等后台 compaction 彻底退出。
  //
  // Env::StartThread/Schedule 出去的任务是 detached 的，DB 析构时无法 join，
  // 所以必须在这里"投一个屏障任务并等它跑完"。Env 的任务队列是 FIFO，屏障
  // 一旦执行，之前排队的 compaction 就一定都已经结束——这时才敢释放 DB 对象。
  // 单靠一个 shutdown_ 标志是不够的：任务可能还没开始跑，标志只是让它提前
  // 返回，并不保证它没在访问 this。
  void WaitForBackgroundCompaction();

  std::vector<size_t> GetLevelFileCounts() const override;
  std::vector<size_t> GetLevelBytes() const override;
  size_t NumTableFiles() const override;

  // 按各层容量阈值挑出一个需要压缩的层；返回 -1 表示当前无需压缩。
  // 优先压 L0（L0 文件互相重叠，对点查最不友好），其次自下而上找超容量的层。
  int PickCompactionLevel(const Version* v) const;

  // level 层的容量上限（字节）。level 越大容量按 multiplier 递增，
  // 这是把"写入总量"摊平成"每层固定大小"的经典做法。
  uint64_t LevelCapacity(int level) const;

  Env* env_;
  const Comparator* user_comparator_;
  InternalKeyComparator icmp_;
  Options options_;  // 持久化层调参（filter_policy / block_size / write_buffer_size）
  std::string dbname_;

  // --- 写路径同步（同 W3）---
  std::mutex mutex_;
  std::deque<Writer*> writers_;  // 等待提交的写请求队列（Group Commit）

  // --- 内存状态 ---
  // 保护 mem_ 的"读取 + 取引用"与"换表 + 释放旧表"这两个动作，
  // 使读者不可能拿到一个正在被 flush 释放的 MemTable。
  // mutable：NewIterator 是 const 方法（它不修改 DB 状态），但仍需在锁内
  // 取 MemTable 的引用。
  mutable std::mutex mem_mutex_;
  std::atomic<MemTable*> mem_{nullptr};  // 活跃 MemTable；flush 时换出
  std::atomic<SequenceNumber> last_sequence_{0};

  // --- WAL ---
  WritableFile* logfile_ = nullptr;  // 活跃 WAL（不拥有，析构时关闭）
  log::Writer* log_ = nullptr;
  uint64_t logfile_number_ = 0;
  std::string wal_name_;

  // --- 版本 / MANIFEST ---
  VersionSet* versions_ = nullptr;  // 拥有

  // --- 后台 compaction ---
  //
  // 【为什么这里没有 condition_variable】
  // 曾经用 mutex + condvar 等后台任务结束，但 condition_variable 的析构
  // 本身就是个坑：它在 ~DBImpl 里销毁，而后台线程的 notify_all 可能恰好
  // 在此刻进行，TSAN 会直接报 data race（即使逻辑上等待者已被唤醒）。
  // 改用 Env 自带的 FIFO 屏障来等待——Env::Schedule 的队列是先进先出，
  // 屏障任务一旦跑到，之前投递的 compaction 就一定都已结束，这是更强的
  // 保证，且不引入需要额外销毁的同步原语。
  std::mutex compaction_mu_;  // 保护 scheduled_ / shutdown_，并保证投递的原子性
  bool compaction_scheduled_ = false;
  bool shutdown_ = false;

  // 正在运行的后台 compaction 数量。
  //
  // 只靠 FIFO 屏障不够：屏障只能保证"排在它前面的任务都跑完了"，而任务从
  // 队列取出到进入 BackgroundCompactionTask 之间存在窗口——屏障可能先于它
  // 执行。显式登记活跃数量，析构时等到归零才是真正的安全点。
  //
  // 增减都在 compaction_mu_ 保护下完成，与 shutdown_ 的判定处于同一临界区，
  // 因此不会出现"析构以为没人跑、任务却刚开始"的空档。
  std::atomic<int> active_compactions_{0};
};

}  // namespace tinystore
