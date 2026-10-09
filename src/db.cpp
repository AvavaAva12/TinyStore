#include "tinystore/db_impl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <set>
#include <vector>

#include "tinystore/internal_key.h"
#include "tinystore/table.h"
#include "tinystore/test_util.h"
#include "tinystore/version_set.h"

namespace tinystore {

namespace {

// Compaction 归并期间借用的一批 Table 引用。
//
// CompactLevel 里有好几条 return 路径（打不开文件、Sync 失败、提交失败……），
// 每条都得归还引用，漏一条就会让 Table 永远无法释放。用 RAII 把这件事收口，
// 声明在 DBIterator 之前即可保证"先析构迭代器、再归还引用"这个顺序——
// 迭代器内部还指着 Table 的索引块，必须活到引用归还之后。
class TableBorrow {
 public:
  explicit TableBorrow(VersionSet* vs) : vs_(vs) {}
  ~TableBorrow() {
    for (uint64_t n : nums_) vs_->ReleaseTable(n);
  }

  TableBorrow(const TableBorrow&) = delete;
  TableBorrow& operator=(const TableBorrow&) = delete;

  void Add(uint64_t n) { nums_.push_back(n); }

 private:
  VersionSet* const vs_;
  std::vector<uint64_t> nums_;
};

// 给 Iterator 包一层：在迭代器销毁时归还它向 VersionSet 借用的 Table 引用。
//
// Iterator 接口没有析构钩子，而 Table 引用必须活到"迭代器不再使用 Table"之后
// ——compaction 会在此期间把旧文件标记淘汰，光靠 Acquire/Release 配对还不够，
// 必须让引用跟着迭代器的生命周期走。
class TableReleasingIterator : public Iterator {
 public:
  TableReleasingIterator(std::unique_ptr<Iterator> inner, VersionSet* vs,
                         std::vector<uint64_t> borrowed)
      : inner_(std::move(inner)), vs_(vs), borrowed_(std::move(borrowed)) {}
  ~TableReleasingIterator() override {
    for (uint64_t n : borrowed_) vs_->ReleaseTable(n);
  }

  TableReleasingIterator(const TableReleasingIterator&) = delete;
  TableReleasingIterator& operator=(const TableReleasingIterator&) = delete;

  bool Valid() const override { return inner_->Valid(); }
  void SeekToFirst() override { inner_->SeekToFirst(); }
  void SeekToLast() override { inner_->SeekToLast(); }
  void Seek(const Slice& target) override { inner_->Seek(target); }
  void Next() override { inner_->Next(); }
  void Prev() override { inner_->Prev(); }
  Slice key() const override { return inner_->key(); }
  Slice value() const override { return inner_->value(); }
  Status status() const override { return inner_->status(); }

 private:
  std::unique_ptr<Iterator> inner_;
  VersionSet* const vs_;
  std::vector<uint64_t> borrowed_;
};

}  // namespace

// ===========================================================================
// DBImpl 实现
// ===========================================================================

DBImpl::DBImpl(const Options& options, const std::string& name)
    : env_(options.env),
      user_comparator_(options.comparator),
      icmp_(user_comparator_),
      options_(options),
      dbname_(name) {
  // 把缓存容量上限交给 VersionSet：它内部按 LRU 淘汰，DB 只提供策略参数。
  versions_ = new VersionSet(dbname_, env_, &icmp_,
                             options_.max_table_cache_bytes);
}

DBImpl::~DBImpl() {
  // 必须先等后台 compaction 彻底结束。它是一个捕获了 this 的后台任务，
  // 若在 DB 析构后仍去访问 versions_ / compaction_mu_，就是 use-after-free。
  WaitForBackgroundCompaction();

  // 兜底 Sync 活跃 WAL（每次 Write 已 fsync，这里只是双保险）
  if (logfile_ != nullptr) {
    logfile_->Sync();
    logfile_->Close();
    delete logfile_;
  }
  delete log_;
  if (MemTable* m = mem_.load(std::memory_order_relaxed)) {
    m->Unref();  // 释放 DB 持有的引用
  }
  delete versions_;

  // 最后释放库级锁。放在最后是刻意的：只要还在清理资源（删版本集、等后台任务），
  // 就不该让另一个进程以为"这个库已经没人用了"而进来抢写。
  // unique_ptr 的析构会调 UnlockFile（flock LOCK_UN + close），
  // 进程被 kill -9 时内核也会自动释放，不会留下死锁。
}

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;

  // ---- 配置校验：把非法配置挡在入口 ----
  //
  // 【为什么不交给各个使用点自己防御】
  // 这些值一旦非法，错误会在离配置点很远的地方以另一种面貌爆出来：比如
  // max_level_bytes_multiplier=0 会让 LevelCapacity 里的 `UINT64_MAX / 0`
  // 在任意一次压缩选层时触发整数除零（UB），而症状是"某次压缩莫名崩溃"，
  // 排查时根本想不到与配置有关。集中校验让问题在 Open 时就暴露。
  if (options.max_level_bytes_multiplier == 0) {
    return Status::InvalidArgument("Options.max_level_bytes_multiplier",
                                   "must be >= 1 (0 would cause divide by zero)");
  }
  if (options.max_num_levels < 2) {
    return Status::InvalidArgument("Options.max_num_levels",
                                   "must be >= 2 (need at least L0 and L1)");
  }
  if (options.write_buffer_size == 0) {
    return Status::InvalidArgument("Options.write_buffer_size", "must be > 0");
  }
  if (options.l0_compaction_trigger == 0) {
    return Status::InvalidArgument("Options.l0_compaction_trigger",
                                   "must be > 0 (0 would compact on every flush)");
  }

  // ---- 目录存在性：create_if_missing / error_if_exists 的组合语义 ----
  //
  // 【此前 create_if_missing 从未被读取】
  // 实现里无条件调用 CreateDirIfMissing，导致用默认 Options{}（该项为 false）
  // 打开一个不存在的库会**静默创建**，与选项语义完全相反 —— 用户以为会拿到
  // NotFound，实际拿到一个空库。若他随后往里写数据，就覆盖了一个本该被拒绝的
  // 场景（例如路径写错，误在别处建了库）。
  const bool dir_exists = options.env->FileExists(name);
  if (!dir_exists) {
    if (!options.create_if_missing) {
      return Status::InvalidArgument(
          name, "does not exist (Options.create_if_missing is false)");
    }
  } else if (options.error_if_exists) {
    return Status::InvalidArgument(name, "exists (Options.error_if_exists is true)");
  }

  DBImpl* impl = new DBImpl(options, name);
  Status s = impl->Recover();
  if (!s.ok()) {
    delete impl;
    return s;
  }
  *dbptr = impl;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Recover：MANIFEST -> Version，重放 WAL -> MemTable，建立可写 WAL
// ---------------------------------------------------------------------------
Status DBImpl::Recover() {
  Status s = env_->CreateDirIfMissing(dbname_);
  if (!s.ok()) return s;

  // 0) 先抢库级排他锁，再碰任何文件。
  //
  // 【顺序为什么重要】
  // 必须放在 CreateDirIfMissing 之后（文件要存在才能 flock），但必须放在
  // VersionSet 恢复**之前**。若反过来先恢复再抢锁，两个进程可能都完成了
  // MANIFEST 重放、进入可写状态，然后第二个进程才抢锁失败——此时它已经把
  // VersionSet 建起来了，必须完整回滚才能不留痕迹。先抢锁则失败得干净。
  //
  // 抢到锁之后 Recover 中途失败也没关系：DBImpl 被 delete，unique_ptr 析构
  // 自动解锁，不会把锁泄漏给后续的重试。
  lock_name_ = Filename::LockFileName(dbname_);
  s = env_->LockFile(lock_name_, &db_lock_);
  if (!s.ok()) {
    return Status::InvalidArgument(
        lock_name_,
        "database is locked by another process (cannot acquire LOCK)");
  }

  // 1) 版本 + MANIFEST 恢复（同时返回存活的 SSTable 编号与当前 WAL 编号）
  std::set<uint64_t> live_files;
  s = versions_->Recover(&live_files);
  if (!s.ok()) return s;

  // 先从 MANIFEST 还原已提交的最大 sequence（flush 后的版本都靠它定快照点）；
  // 之后若重放 WAL，再取二者较大值。
  last_sequence_.store(versions_->LastSequence(), std::memory_order_relaxed);

  // 2) 清理孤儿文件：不在当前版本的 .ldb，以及不是当前 WAL 的 .log
  std::vector<std::string> children;
  if (env_->GetChildren(dbname_, &children).ok()) {
    for (const auto& name : children) {
      uint64_t num = 0;
      if (name == "MANIFEST") continue;
      if (!Filename::ParseFileName(name, &num)) continue;
      if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".ldb") == 0) {
        if (live_files.find(num) == live_files.end()) {
          env_->DeleteFile(dbname_ + "/" + name);
        }
      } else if (name.size() >= 4 &&
                 name.compare(name.size() - 4, 4, ".log") == 0) {
        if (num != versions_->log_number()) {
          env_->DeleteFile(dbname_ + "/" + name);
        }
      }
    }
  }

  // 3) 建 MemTable（DB 持有其引用）
  MemTable* m = new MemTable(&icmp_);
  m->Ref();

  // 4) 重放当前 WAL 到 MemTable
  const uint64_t log_num = versions_->log_number();
  if (log_num != 0) {
    wal_name_ = Filename::MakeFileName(dbname_, log_num, "log");
    if (!env_->FileExists(wal_name_)) {
      m->Unref();
      return Status::IOError(wal_name_, "current log file missing");
    }
    std::unique_ptr<SequentialFile> file;
    s = env_->NewSequentialFile(wal_name_, &file);
    if (!s.ok()) {
      m->Unref();
      return s;
    }
    log::Reader reader(file.get(), nullptr, true);
    std::string scratch;
    Slice record;
    SequenceNumber max_seq = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      WriteBatch batch;
      batch.SetContents(record);
      const SequenceNumber first = WriteBatchInternal::Sequence(&batch);
      const uint32_t n = batch.Count();
      if (n > 0) {
        // 损坏记录一条都插不进去，此时**不能**推进 max_seq，否则快照点会被推到
        // 一个从未写入的区间（sequence 空洞），这批数据也静默丢失。
        s = WriteBatchInternal::InsertInto(&batch, m);
        if (!s.ok()) break;
        max_seq = std::max(max_seq, first + n - 1);
      }
    }
    if (!s.ok()) {
      m->Unref();
      return s;
    }
    // 区分"读完"与"损坏"：WAL 中段损坏必须硬失败，不能当成正常读完继续开库。
    // 尾部残缺（写到一半崩溃）由 Reader 归为正常 EOF，status() 为 OK，可安全继续。
    if (!reader.status().ok()) {
      m->Unref();
      return reader.status();
    }
    // 与 MANIFEST 里的 sequence 取较大值：flush 后的数据不在本 WAL 中，
    // 只靠重放 WAL 会拿回一个偏小（甚至为 0）的快照点。
    last_sequence_.store(
        std::max(last_sequence_.load(std::memory_order_relaxed), max_seq),
        std::memory_order_relaxed);

    // 以续写模式打开同一个 WAL（崩溃恢复后继续追加，不截断已有数据）
    uint64_t fsize = 0;
    env_->GetFileSize(wal_name_, &fsize);
    std::unique_ptr<WritableFile> wfile;
    s = env_->NewAppendableFile(wal_name_, &wfile);
    if (!s.ok()) {
      m->Unref();
      return s;
    }
    logfile_ = wfile.release();
    log_ = new log::Writer(logfile_, fsize % log::kBlockSize);
    logfile_number_ = log_num;
  }

  mem_.store(m, std::memory_order_release);

  // 5) 全新库（无 WAL）：创建第一个 WAL 并登记到 MANIFEST
  if (logfile_ == nullptr) {
    const uint64_t new_num = versions_->NewFileNumber();
    wal_name_ = Filename::MakeFileName(dbname_, new_num, "log");
    std::unique_ptr<WritableFile> wfile;
    s = env_->NewWritableFile(wal_name_, &wfile);
    if (!s.ok()) {
      m->Unref();
      return s;
    }
    logfile_ = wfile.release();
    log_ = new log::Writer(logfile_);
    logfile_number_ = new_num;

    VersionEdit edit;
    edit.log_number = new_num;
    edit.has_log_number = true;
    edit.sequence = last_sequence_.load(std::memory_order_relaxed);
    edit.has_sequence = true;
    s = versions_->LogAndApply(&edit);
    if (!s.ok()) {
      m->Unref();
      return s;
    }
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// CompactMemTable：flush 活跃 MemTable -> SSTable，并滚动 WAL
// ---------------------------------------------------------------------------
Status DBImpl::CompactMemTable() {
  MemTable* imm = mem_.load(std::memory_order_acquire);
  if (imm == nullptr) return Status::OK();

  // 1) 把 MemTable 写成一个 SSTable
  const uint64_t file_number = versions_->NewFileNumber();
  const std::string fname = Filename::MakeFileName(dbname_, file_number, "ldb");
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(fname, &file);
  if (!s.ok()) return s;

  TableBuilder builder(&icmp_, file.get(), options_.filter_policy,
                       options_.block_size);
  {
    MemTable::Iterator iter(imm);
    for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
      builder.Add(iter.internal_key(), iter.value());
    }
  }
  s = builder.Finish();
  if (s.ok()) s = file->Sync();  // SSTable 必须落盘，flush 才有意义
  if (!s.ok()) {
    env_->DeleteFile(fname);
    return s;
  }
  // 【为什么要检查 Close 的返回值】
  // 上面只 Sync 过数据内容，Close 还负责收尾（刷出剩余缓冲、归还文件描述符）。
  // 它失败意味着这个 SSTable 可能并不完整；此时若继续把它登记进 MANIFEST，
  // 就会在版本里留下一个打不开的文件，后续读该区间会直接失败。
  s = file->Close();
  if (!s.ok()) {
    env_->DeleteFile(fname);
    return s;
  }
  file.reset();

  // 崩溃注入点：SSTable 已完整落盘，但 MANIFEST 里还没有它。
  // 此刻这个 .ldb 是孤儿——恢复时必须被清理，且数据仍能从旧 WAL 重放出来。
  // 若恢复逻辑误把它当成有效文件，就会读到一份"存在但从未被承认"的数据。
  MaybeCrashForTesting(kCrashPointAfterFlushSstWrite);

  const uint64_t file_size = builder.FileSize();
  const std::string smallest = builder.SmallestKey();
  const std::string largest = builder.LargestKey();

  // 2) 启动新的 WAL（flush 之后的新写都进这里）
  const uint64_t new_log = versions_->NewFileNumber();
  const std::string new_wal = Filename::MakeFileName(dbname_, new_log, "log");
  std::unique_ptr<WritableFile> new_wfile;
  s = env_->NewWritableFile(new_wal, &new_wfile);
  if (!s.ok()) {
    env_->DeleteFile(fname);
    return s;
  }

  // 3) 提交版本变更（新增 SSTable + 切换到新 WAL）—— flush 的原子点
  VersionEdit edit;
  FileMetaData meta;
  meta.number = file_number;
  meta.file_size = file_size;
  meta.smallest = smallest;
  meta.largest = largest;
  edit.new_files.push_back(meta);
  edit.log_number = new_log;
  edit.has_log_number = true;
  // 已提交的最大 sequence：这次 flush 之后 WAL 会被滚动成空文件，
  // 恢复时只能靠 MANIFEST 里的这个值还原快照点。
  edit.sequence = last_sequence_.load(std::memory_order_relaxed);
  edit.has_sequence = true;
  s = versions_->LogAndApply(&edit);
  if (!s.ok()) {
    env_->DeleteFile(fname);
    return s;
  }

  // 崩溃注入点：MANIFEST 已提交，但旧 WAL 还在、新 WAL 还没建。
  // 数据此时"两头都有"（SSTable 已登记、旧 WAL 未删），恢复必须容忍这种重叠：
  // 重放旧 WAL 会得到重复记录，靠 sequence 去重即可，不能报错也不能丢。
  MaybeCrashForTesting(kCrashPointAfterFlushCommit);

  // 4) 滚动 WAL：旧 WAL 的数据已经在 SSTable 里，可安全删除
  const std::string old_wal = wal_name_;
  if (logfile_ != nullptr) {
    logfile_->Close();
    delete logfile_;
  }
  delete log_;
  logfile_ = new_wfile.release();
  log_ = new log::Writer(logfile_);
  wal_name_ = new_wal;
  logfile_number_ = new_log;
  env_->DeleteFile(old_wal);

  // 5) 切换 MemTable 到新的空表；旧表由正在读的线程 Ref 保活，这里释放 DB 的引用
  MemTable* new_mem = new MemTable(&icmp_);
  new_mem->Ref();
  {
    // 换表与"释放旧表的 DB 引用"必须与 Get() 的"load + Ref"在同一把锁内完成：
    // 否则读者可能刚 load 到 imm、还没来得及 Ref，imm 就被这里 Unref 到 0
    // 而 delete，读者随后对悬垂指针操作跳表 —— use-after-free。
    std::lock_guard<std::mutex> lk(mem_mutex_);
    mem_.store(new_mem, std::memory_order_relaxed);
    imm->Unref();
  }

  return Status::OK();
}

// ---------------------------------------------------------------------------
// Compaction：把 level 层的文件与 level+1 层重叠文件归并成新的 level+1 文件
// ---------------------------------------------------------------------------
//
// 【为什么必须有这一步】
// flush 只会新增 SSTable、从不删除。写 1M 条数据就会得到上千个文件，而点查
// 最坏要把它们全看一遍（布隆过滤器能跳过大部分，但文件本身的打开、读 footer、
// 读索引块都是实打实的 IO）。Compaction 把多个文件合并成少数几个，文件数因此
// 从"正比于写入总量"降到"正比于数据总量 / 单文件大小"。
//
// 【L0 与更高层的区别】
// L0 文件来自 flush，键范围互相重叠，所以查询要整层扫；更高层由 compaction
// 产出，文件之间键范围互不重叠，查询可以二分定位到唯一文件。
uint64_t DBImpl::LevelCapacity(int level) const {
  if (level <= 0) {
    // L0 不用容量触发，只用文件数触发（见 NeedsCompaction）：
    // L0 的问题在"文件多且重叠"而非"字节多"。
    return 0;
  }
  uint64_t cap = options_.max_level_bytes;
  for (int i = 1; i < level; ++i) {
    if (cap > UINT64_MAX / options_.max_level_bytes_multiplier) {
      return UINT64_MAX;  // 防溢出：层数很深时直接封顶
    }
    cap *= options_.max_level_bytes_multiplier;
  }
  return cap;
}

// 按各层"压力"挑出一个最需要压缩的层；返回 -1 表示当前无需压缩。
//
// 【为什么要按压力排序，而不是"固定先 L0，再自下而上找第一个"】
// 固定顺序隐含一个假设：L0 超了就是最紧急的。但真实负载下多层可能同时超限，
// 固定顺序会让靠后的层**长期得不到压缩** —— 每一轮都在处理更紧急的 L0，
// 底层无限增长，读放大持续恶化，只是恶化得慢一点。
//
// 改为比较"超限倍数"（实际值 / 阈值，取最大者）后，压力最大的层总是先被处理，
// 不存在某个层被无限期跳过的情况。
//
// 【并列时偏向小 level】
// 严格大于才替换，因此遍历顺序（level 升序）天然让 L0 在同压力下优先。
// 理由：L0 文件互相键范围重叠，点查要整层扫，对延迟最不友好；高层可以二分
// 定位到唯一文件，同样压力的代价更小。
int DBImpl::PickCompactionLevel(const Version* v) const {
  int best = -1;
  double best_pressure = 1.0;  // 未超过阈值（1.0）不算需要压缩

  for (int lvl = 0; lvl < options_.max_num_levels; ++lvl) {
    double pressure = 0.0;
    if (lvl == 0) {
      // L0 不用字节数触发：它的产出速率由写入决定，问题在"文件多且互相重叠"，
      // 用文件数衡量更贴近点查的真实代价（每多一个 L0 文件就多一次
      // 打开 + 读 footer + 读索引）。
      pressure = static_cast<double>(v->NumLevelFiles(0)) /
                 static_cast<double>(options_.l0_compaction_trigger);
    } else {
      const uint64_t cap = LevelCapacity(lvl);
      if (cap == 0) continue;
      pressure = static_cast<double>(v->LevelBytes(lvl)) /
                 static_cast<double>(cap);
    }
    if (pressure > best_pressure) {
      best_pressure = pressure;
      best = lvl;
    }
  }
  return best;
}

// 当前是否应当**暂缓**压缩（限流）。
//
// 【限流的必要性】
// Compaction 与前台写共享同一条磁盘带宽。若它长期跑得比写入快很多，会持续
// 抢占 IO —— 表现为前台写延迟抬升，而 CPU 和内存都在空转。用一个"写放大"
// 上限把压缩的总产出约束住，让它只能在写入让出的那部分带宽里做功。
//
// 【为什么必须有逃生阀】
// 只按写放大限流有一个致命副作用：**纯读负载下压缩会被永久推迟**。
// 极端情况下写入为 0、预算也为 0，压缩一次都做不了，而此时 L0 可能正堆积着
// 上次退出时留下的文件，读放大只会越来越糟。
//
// 所以叠加一个积压判据：L0 文件数一旦超过 l0_trigger × backlog_factor，
// 说明已经"欠了太多"，此时无视限流强制压缩。
//
// 【为什么用累计字节数而不是速率】
// 速率（字节/秒）需要维护滑动窗口，且在测试里难以稳定复现；累计字节数
// 与"写放大"这个 LSM 的标准指标直接对应，写放大 = 总产出 / 用户写入，
// 限流即"写放大不超过配置值"，语义清晰且与磁盘状态无关。
bool DBImpl::CompactionThrottled(const Version* v) const {
  const int factor =
      options_.compaction_backlog_factor > 0 ? options_.compaction_backlog_factor : 4;
  const uint64_t backlog_cap =
      static_cast<uint64_t>(options_.l0_compaction_trigger) *
      static_cast<uint64_t>(factor);
  // 逃生阀：积压到硬上限，强制压缩，不再看预算。
  if (static_cast<uint64_t>(v->NumLevelFiles(0)) >= backlog_cap) return false;

  const uint64_t budget =
      bytes_written_.load(std::memory_order_relaxed) *
      options_.compaction_max_write_amplification;
  return compaction_bytes_written_.load(std::memory_order_relaxed) >= budget;
}

// 判断 [a.smallest, a.largest] 与 [b.smallest, b.largest] 是否键范围重叠。
static bool RangesOverlap(const InternalKeyComparator* icmp,
                          const FileMetaData& a, const FileMetaData& b) {
  // a 的最小 > b 的最大  =>  a 全在 b 右边
  if (icmp->Compare(a.smallest, b.largest) > 0) return false;
  // a 的最大 < b 的最小  =>  a 全在 b 左边
  if (icmp->Compare(a.largest, b.smallest) < 0) return false;
  return true;
}

Status DBImpl::CompactLevel(int level) {
  // 【拒绝压缩最后一层】
  // PickCompactionLevel 的循环范围是 [0, max_num_levels)，所以 level 最大可以是
  // max_num_levels-1，压缩后输出会落到第 max_num_levels 层。而该层永远不会被
  // PickCompactionLevel 选中（循环不覆盖它），这批文件就永久不再参与压缩，
  // 读放大持续上升。实测需要 8TB 数据才触发，属理论缺陷，但边界必须守住。
  if (level < 0 || level + 1 >= options_.max_num_levels) {
    return Status::InvalidArgument("CompactLevel", "level out of range");
  }

  const int next_level = level + 1;
  const bool next_is_final = (next_level >= options_.max_num_levels - 1);

  // 1) 选源文件。L0 整体参与；更高层只挑一个文件（挑最小的那个最划算，
  //    因为它归并后能最快沉到下一层）。
  //
  // 【为什么按值拷贝而不是存 const FileMetaData*】
  // VersionSet 换版本时 old->Unref() 会在归零时 delete 旧 Version。若这里存
  // 元素的裸指针，Unref() 之后 VersionSet::current() 返回的裸指针，以及下面
  // min_element / input_numbers / RangesOverlap 的每一次解引用，都可能读到
  // 已释放内存。W7 把 compaction 挪到后台后，flush（写路径线程）与 compaction
  // （后台线程）会并发 LogAndApply，**这个窗口从边缘场景变成了常态**。
  //
  // FileMetaData 只含 3 个 string + 2 个数字，拷贝成本远低于一次 compaction
  // 的 IO，用内存换正确性是划算的。
  std::vector<FileMetaData> inputs;
  {
    Version* v = versions_->current();
    if (v == nullptr) return Status::OK();
    if (v->files().empty()) {
      v->Unref();
      return Status::OK();
    }
    for (const FileMetaData* f : v->FilesAtLevel(level)) inputs.push_back(*f);
    v->Unref();
  }
  if (inputs.empty()) return Status::OK();
  if (level > 0 && inputs.size() > 1) {
    // 先拷进局部变量再 assign：min_element 返回的是 inputs 内元素的引用，
    // 边赋值边读同一个 vector 会踩迭代器失效。
    const FileMetaData smallest =
        *std::min_element(inputs.begin(), inputs.end(),
                          [](const FileMetaData& a, const FileMetaData& b) {
                            return a.number < b.number;
                          });
    inputs.assign(1, smallest);
  }

  // 2) 选下一层中与源文件范围重叠的文件。这些文件要被源文件的新版本覆盖，
  //    所以必须一起参与归并，否则旧版本会"复活"。
  //
  // 只需记录编号：归并阶段按 input_numbers 逐个 AcquireTable（见下文第 3 步），
  // 不需要再持有 FileMetaData 本身。
  std::vector<uint64_t> input_numbers;
  {
    Version* v = versions_->current();
    if (v == nullptr) return Status::OK();
    for (const FileMetaData& f : inputs) input_numbers.push_back(f.number);
    for (const FileMetaData* gp : v->FilesAtLevel(next_level)) {
      const FileMetaData& g = *gp;
      bool overlaps = false;
      for (const FileMetaData& f : inputs) {
        if (RangesOverlap(&icmp_, f, g)) {
          overlaps = true;
          break;
        }
      }
      if (overlaps) input_numbers.push_back(g.number);
    }
    v->Unref();
  }

  // 3) 归并。用 DBIterator 串起所有输入源：它按 internal_key 有序归并，并按
  //    compaction 规则决定哪些版本该留（见 DBIterator 的"Compaction 模式"）。
  //
  //    snapshot 传**最早活跃快照**（无活跃快照时是 kMaxSequenceNumber）：
  //    * 无活跃快照时 = kMaxSequenceNumber，每个 user_key 只留最新版，
  //      写放大最低——这是 W6 以来一直的行为；
  //    * 有活跃快照时保留该快照依赖的版本，历史读才不会在压缩中途失效。
  //
  //    drop_tombstones 只在最底层为 true：中间层必须让墓碑继续下沉，
  //    否则该 key 在更深层的旧版本会失去遮挡而"复活"（W6 遗留的缺陷）。
  std::vector<FileMetaData> outputs;
  {
    // borrow 先声明、merged 后声明 => 析构时 merged 先走、borrow 再归还引用。
    TableBorrow borrow(versions_);
    DBIterator merged(&icmp_, EarliestSnapshot(), /*for_compaction=*/true,
                      /*drop_tombstones=*/next_is_final);
    for (uint64_t num : input_numbers) {
      Table* t = versions_->AcquireTable(num);
      if (t == nullptr) continue;  // 打不开就跳过，与 Get 的处理一致
      borrow.Add(num);
      merged.AddChild(t->NewIterator());
    }
    std::unique_ptr<WritableFile> out_file;
    std::unique_ptr<TableBuilder> builder;
    uint64_t out_number = 0;

    // 开始一个新输出文件
    auto start_output = [&]() -> Status {
      out_number = versions_->NewFileNumber();
      const std::string fname =
          Filename::MakeFileName(dbname_, out_number, "ldb");
      std::unique_ptr<WritableFile> f;
      Status s = env_->NewWritableFile(fname, &f);
      if (!s.ok()) return s;
      out_file = std::move(f);
      builder = std::make_unique<TableBuilder>(&icmp_, out_file.get(),
                                               options_.filter_policy,
                                               options_.block_size);
      return Status::OK();
    };

    // 收尾当前输出文件并登记元数据
    auto finish_output = [&]() -> Status {
      if (builder == nullptr) return Status::OK();
      FileMetaData meta;
      meta.number = out_number;
      meta.level = next_level;
      // 注意：空输出（例如全部键都被删光）是合法且必要的结果——
      // compaction 正是回收墓碑、把空间还给系统的地方。
      if (builder->NumEntries() == 0) {
        env_->DeleteFile(Filename::MakeFileName(dbname_, out_number, "ldb"));
      } else {
        Status s = builder->Finish();
        if (s.ok()) s = out_file->Sync();
        if (!s.ok()) {
          env_->DeleteFile(Filename::MakeFileName(dbname_, out_number, "ldb"));
          return s;
        }
        meta.file_size = builder->FileSize();
        meta.smallest = builder->SmallestKey();
        meta.largest = builder->LargestKey();
        // 计入写放大统计：限流靠它与用户写入量比较（见 CompactionThrottled）。
        compaction_bytes_written_.fetch_add(meta.file_size,
                                            std::memory_order_relaxed);
        outputs.push_back(std::move(meta));
      }
      out_file.reset();
      builder.reset();
      return Status::OK();
    };

    merged.SeekToFirst();
    Status s;
    for (; merged.Valid(); merged.Next()) {
      if (!s.ok()) s = merged.status();
      if (!s.ok()) break;
      if (builder == nullptr) {
        s = start_output();
        if (!s.ok()) break;
      }
      // 必须写 internal key 而不是 merged.key()（那是 user_key）：
      // SSTable 的键空间是 internal key，缺了 8 字节的 seq/type 后缀，
      // TableBuilder 内部的 FindShortestSeparator 会把它当 internal key 解析而断言失败，
      // 更关键的是压缩后版本号丢失、快照读直接失效。
      builder->Add(merged.internal_key(), merged.value());
      // 写满一个目标大小就切分下一个文件，避免产出巨型 SSTable
      if (builder->FileSize() >= options_.max_compaction_file_size) {
        s = finish_output();
        if (!s.ok()) break;
      }
    }
    if (s.ok() && builder != nullptr) {
      s = finish_output();
    }
    if (!s.ok()) {
      // 失败清理：把本次已产出的文件删掉，避免留下未登记的孤儿
      for (const auto& m : outputs) {
        env_->DeleteFile(Filename::MakeFileName(dbname_, m.number, "ldb"));
      }
      return s;
    }
  }

  // 崩溃注入点：输出文件已全部落盘，但 MANIFEST 里既没登记它们、
  // 也没声明删除源文件。恢复后源文件必须仍然有效——若恢复逻辑看到输出文件
  // 就以为"压缩已完成"，会把真正持有数据的源文件当成孤儿删掉，直接丢数据。
  MaybeCrashForTesting(kCrashPointAfterCompactionOutput);

  // 4) 原子提交：MANIFEST 里同时记录"新增输出文件"和"删除源文件"。
  //    这条记录落盘之前崩溃，旧文件仍在、新文件只是孤儿，重启后会被清理；
  //    落盘之后崩溃，两边都已登记，数据完整。绝不能分两次提交。
  VersionEdit edit;
  edit.sequence = last_sequence_.load(std::memory_order_relaxed);
  edit.has_sequence = true;
  for (const auto& m : outputs) edit.new_files.push_back(m);
  for (uint64_t num : input_numbers) edit.deleted_files.push_back(num);

  Status s = versions_->LogAndApply(&edit);
  if (!s.ok()) {
    for (const auto& m : outputs) {
      env_->DeleteFile(Filename::MakeFileName(dbname_, m.number, "ldb"));
    }
    return s;
  }

  // 5) 新版本已生效，此刻才安全地删旧文件：必须先从 table_cache_ 摘除
  //    （EvictTable），否则缓存里会留下指向已删文件的悬垂 Table*。
  for (uint64_t num : input_numbers) {
    versions_->EvictTable(num);
    env_->DeleteFile(Filename::MakeFileName(dbname_, num, "ldb"));
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 后台 compaction：写路径只负责"判断 + 投任务"，IO 全在后台线程
// ---------------------------------------------------------------------------

void DBImpl::MaybeScheduleCompaction() {
  // 占位、判断、投递放在同一个临界区：这样"已投递"与"已占位"严格等价，
  // 析构时才能用 active_compactions_ 判断"没有任务在跑"。
  std::lock_guard<std::mutex> lk(compaction_mu_);
  if (shutdown_) return;
  if (compaction_scheduled_) return;

  int trigger = -1;
  Version* cur = versions_->current();
  if (cur != nullptr) {
    trigger = PickCompactionLevel(cur);
    cur->Unref();
  }
  if (trigger < 0) return;

  compaction_scheduled_ = true;
  env_->Schedule([this] { BackgroundCompactionTask(); });
}

void DBImpl::BackgroundCompactionTask() {
  {
    std::lock_guard<std::mutex> lk(compaction_mu_);
    if (shutdown_) {
      // 投递到析构之间可能已经关库：清占位后立刻返回，别再碰任何成员。
      compaction_scheduled_ = false;
      return;
    }
    // 登记"我正在跑"。与 shutdown_ 的判定同处一把锁、同一临界区，
    // 因此不存在"析构以为没人跑、其实任务已开始"的窗口。
    ++active_compactions_;
  }

  // 循环压到收敛：一次压缩可能又把下一层顶过阈值，循环才能真正停下来。
  for (int guard = 0; guard < 64; ++guard) {
    int trigger = -1;
    bool throttled = false;
    {
      Version* cur = versions_->current();
      if (cur != nullptr) {
        trigger = PickCompactionLevel(cur);
        if (trigger >= 0) throttled = CompactionThrottled(cur);
        cur->Unref();
      }
    }
    if (trigger < 0) break;
    // 被限流就退出本轮。下次写入会让预算增长、进而重新触发，
    // 所以不会"忘了压"——若真到了积压上限，CompactionThrottled 本身就返回 false。
    if (throttled) break;
    Status s = CompactLevel(trigger);
    if (!s.ok()) break;  // 出错就停手，等下次写入再触发
  }

  {
    std::lock_guard<std::mutex> lk(compaction_mu_);
    compaction_scheduled_ = false;
    // 归零放在最后一步：之后任务不再访问任何 DB 成员，析构等到 0 即为安全点。
    --active_compactions_;
  }
}

void DBImpl::WaitForBackgroundCompaction() {
  {
    std::unique_lock<std::mutex> lk(compaction_mu_);
    shutdown_ = true;
    // 等到没有任务在跑。这里刻意先释放锁再忙等：任务要拿同一把锁才能把
    // active_compactions_ 减到 0，持锁等待会直接死锁。
    //
    // 不用 condition_variable 的原因：它需要在 ~DBImpl 里销毁，而后台线程
    // 可能恰好在 notify_all，TSAN 会把这种"逻辑上已唤醒"的并发判为 data race。
    // 忙等只依赖析构方独占的这把锁，不引入需要额外销毁的同步原语。
    while (active_compactions_.load(std::memory_order_acquire) > 0) {
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      lk.lock();
    }
  }
  // 再投一个屏障任务：Env 的队列是 FIFO，它一旦跑到，就说明排在前面的
  // compaction 都已执行完毕，此后不会再有任务访问 this。
  std::promise<void> barrier;
  env_->Schedule([&barrier] { barrier.set_value(); });
  barrier.get_future().wait();
}

// ---------------------------------------------------------------------------
// 统计与自省：让 Compaction 的效果可被测试直接断言
// ---------------------------------------------------------------------------

std::vector<size_t> DBImpl::GetLevelFileCounts() const {
  Version* v = versions_->current();
  std::vector<size_t> out;
  for (int lvl = 0; lvl < options_.max_num_levels; ++lvl) {
    out.push_back(static_cast<size_t>(v->NumLevelFiles(lvl)));
  }
  v->Unref();
  return out;
}

std::vector<size_t> DBImpl::GetLevelBytes() const {
  Version* v = versions_->current();
  std::vector<size_t> out;
  for (int lvl = 0; lvl < options_.max_num_levels; ++lvl) {
    out.push_back(static_cast<size_t>(v->LevelBytes(lvl)));
  }
  v->Unref();
  return out;
}

size_t DBImpl::NumTableFiles() const {
  Version* v = versions_->current();
  const size_t n = v->files().size();
  v->Unref();
  return n;
}

size_t DBImpl::TableCacheEntries() const { return versions_->CacheEntries(); }

uint64_t DBImpl::TableCacheBytes() const { return versions_->CacheBytes(); }

// ---------------------------------------------------------------------------
// Write：Group Commit（同 W3）+ 阈值触发 flush
// ---------------------------------------------------------------------------
Status DBImpl::Write(const WriteBatch& my_batch) {
  Writer w;
  w.batch = &my_batch;
  w.done = false;

  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&w);

  // 不是队首 -> 已有 leader 在提交，等它唤醒（leader 会把本请求一起提交）
  while (!w.done && &w != writers_.front()) {
    w.cv.wait(lock);
  }
  if (w.done) {
    return w.status;
  }

  // ---- 我是 leader：合并队列里所有等待者成一个大 batch ----
  WriteBatch combined;
  for (Writer* it : writers_) {
    if (it->batch != nullptr && it->batch->Count() > 0) {
      WriteBatchInternal::Append(&combined, it->batch);
    }
  }

  if (combined.Count() == 0) {
    while (!writers_.empty()) {
      Writer* ready = writers_.front();
      writers_.pop_front();
      ready->done = true;
      ready->cv.notify_one();
    }
    return Status::OK();
  }

  // 分配本组起始 sequence，写进 WAL，整组只 fsync 一次
  const SequenceNumber seq = last_sequence_.load(std::memory_order_relaxed) + 1;
  WriteBatchInternal::SetSequence(&combined, seq);

  Status s = log_->AddRecord(combined.Contents());
  if (s.ok()) s = logfile_->Sync();  // Group Commit 关键：一次 fsync 摊薄整组

  // 崩溃注入点：WAL 已落盘、内存视图尚未更新。
  // 这是"已对用户承诺成功但内存里看不到"的窗口，恢复必须靠 WAL 重放兜住。
  MaybeCrashForTesting(kCrashPointAfterWalSync);

  if (s.ok()) {
    MemTable* m = mem_.load(std::memory_order_acquire);
    // 自己刚构造的 batch 不可能损坏，这里 OK 即忽略；WAL 重放路径才必须检查
    WriteBatchInternal::InsertInto(&combined, m);
    // 先回放内存，再 release 发布新的 last_sequence_
    last_sequence_.store(seq + combined.Count() - 1, std::memory_order_release);

    // 用户写入量：compaction 限流的预算基准（见 CompactionThrottled）。
    // 用 batch 编码后的长度而非解压后的 key+value——它正比于实际数据量，
    // 却不需要再遍历一遍 batch，且包含了写放大统计本来就该计入的协议开销。
    bytes_written_.fetch_add(combined.Contents().size(),
                             std::memory_order_relaxed);

    // W4：MemTable 涨过阈值则 flush（在持有 mutex_ 的 leader 里同步完成）
    if (m->ApproximateMemoryUsage() > options_.write_buffer_size) {
      s = CompactMemTable();
    }

    // W7：flush 之后看看要不要压缩，但只"投个任务"就返回。
    // 真正的归并在后台线程做，写路径不做 IO，因此不会被压缩拖住。
    if (s.ok()) {
      MaybeScheduleCompaction();
    }
  }

  while (!writers_.empty()) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = s;
    }
    ready->done = true;
    ready->cv.notify_one();
  }
  return s;
}

Status DBImpl::Put(const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(batch);
}

Status DBImpl::Delete(const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(batch);
}

// ---------------------------------------------------------------------------
// Get：无锁快照读（MemTable -> SSTable，从新到旧）
// ---------------------------------------------------------------------------
Status DBImpl::Get(const Slice& key, std::string* value) {
  return Get(key, value, ReadOptions());
}

// ---------------------------------------------------------------------------
// 快照句柄：登记 / 注销，并回答"最早的活跃快照是谁"
// ---------------------------------------------------------------------------
const Snapshot* DBImpl::GetSnapshot() {
  // 快照点取"此刻已提交的最大 sequence"：它之后写入的内容对本次读不可见。
  // 用 acquire 语义读，与写路径发布 last_sequence_ 的 release 配对，
  // 确保能看到该 sequence 之前的所有写入（否则刚写完立刻取快照会读不到）。
  const SequenceNumber seq = last_sequence_.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lk(snapshot_mu_);
    ++snapshots_[seq];
  }
  return new Snapshot(seq);
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return;
  {
    std::lock_guard<std::mutex> lk(snapshot_mu_);
    const uint64_t seq = snapshot->sequence();
    auto it = snapshots_.find(seq);
    // 找不到说明调用方重复释放了同一个句柄，属于用法错误。
    // 不崩（返回即可），但删掉快照对象——否则调用方拿它再放一次就是 UAF。
    if (it != snapshots_.end()) {
      if (--it->second == 0) snapshots_.erase(it);
    }
  }
  delete snapshot;
}

SequenceNumber DBImpl::EarliestSnapshot() const {
  std::lock_guard<std::mutex> lk(snapshot_mu_);
  if (snapshots_.empty()) return kMaxSequenceNumber;
  // std::map 按 key 有序，最小键即最早快照。
  return static_cast<SequenceNumber>(snapshots_.begin()->first);
}

Status DBImpl::Get(const Slice& key, std::string* value,
                   const ReadOptions& options) {
  // 1) 取活跃 MemTable 的引用。
  //    临界区内完成 load + Ref：mem_ 恒有 DB 持有的所有权引用，所以此刻
  //    load 到的表必然存活，Ref 一定成功（对比旧的"load + TryRef 重试"写法，
  //    那个写法在对象已被 delete 时会对悬垂指针调用 TryRef，属 use-after-free）。
  //    锁内只有指针获取与引用计数，真正的数据读取全部在锁外。
  MemTable* m = nullptr;
  {
    std::lock_guard<std::mutex> lk(mem_mutex_);
    m = mem_.load(std::memory_order_relaxed);
    if (m != nullptr) m->Ref();
  }
  if (m == nullptr) return Status::NotFound("db not ready");

  // 快照点解析：用户传的值必须被截断到"当前已提交的最大 sequence"。
  //
  // 【为什么不能直接用 options.snapshot】
  // 默认值是 kMaxSequenceNumber（意为"读最新"）。但写路径是"先把 batch 插进
  // MemTable、再 release 发布 last_sequence_"，这两步之间存在窗口：另一个线程
  // 此时读 kMaxSequenceNumber，会看见 MemTable 里 seq > last_sequence_ 的条目——
  // 也就是**尚未确认提交的数据**。若那批写入随后失败（比如 fsync 报错），读者
  // 就看到了一个从未真正提交的值。
  //
  // 所以默认走 last_sequence_（严格只读已提交数据）；用户显式传了更小的值时，
  // 取两者较小者：既能做历史读，又绝不会越过"已提交"这条线。
  SequenceNumber snapshot = last_sequence_.load(std::memory_order_acquire);
  if (options.snapshot < snapshot) snapshot = options.snapshot;

  bool found = false;
  Status s = m->Get(key, snapshot, value, &found);
  if (found) {  // 命中（值或删除）-> 不必再查 SSTable
    m->Unref();
    return s;
  }
  // 未命中：后续只查 SSTable，不再需要 MemTable 的引用，提前释放。
  m->Unref();

  // 【与下面 SSTable 侧同一原则】非 NotFound 的错误必须上报，不能继续扫。
  // MemTable::Get 在无法解析 internal key 时返回 Corruption 且 found 保持 false，
  // 若在此静默丢弃，内存损坏就被伪装成"key 不存在"，与整条读路径的既定原则矛盾。
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  // 2) SSTable：从新到旧扫描；每个文件的 bloom 过滤器可整体跳过"一定不含"的文件
  Version* v = versions_->current();
  if (v == nullptr) {
    // 防御性检查：current() 在 current_ 为空时返回 nullptr。
    return Status::Corruption("db: no current version");
  }
  const FilterPolicy* fp = options_.filter_policy;
  for (int i = static_cast<int>(v->files().size()) - 1; i >= 0; --i) {
    const FileMetaData& f = v->files()[i];
    Table* table = versions_->AcquireTable(f.number);
    // AcquireTable 拿不到时只能跳过：可能是文件已被 compaction 淘汰
    // （此时新版本里有它的替身），也可能文件确实打不开。
    if (table == nullptr) continue;
    bool tfound = false;
    Status ts = table->Get(key, snapshot, fp, value, &tfound);
    // 用完立刻归还引用：否则 compaction 淘汰该文件时，这个 Table 永远等不到
    // 引用归零，既释放不掉内存，也会让缓存无限膨胀。
    versions_->ReleaseTable(f.number);
    if (tfound) {  // 本文件确有该 key 在 snapshot 下的版本（值或删除）-> 停止
      v->Unref();
      return ts;
    }
    // 【为什么非 NotFound 的错误必须上报，不能继续扫】
    // NotFound 表示"这个文件里没有该 key 的任何版本"，继续找更老的文件是对的。
    // 但 Corruption（块 CRC 不匹配、footer 非法、内部键无法解析）说明磁盘数据
    // 已损坏——若一并当成 NotFound 吞掉，损坏会被静默伪装成"key 不存在"，
    // 线上排障时看不到任何痕迹。这正是本项目要极力避免的"静默失败"。
    if (!ts.ok() && !ts.IsNotFound()) {
      v->Unref();
      return ts;
    }
  }

  v->Unref();
  return Status::NotFound("not found");
}

// ---------------------------------------------------------------------------
// NewIterator：跨 MemTable 与全部 SSTable 的归并迭代器
// ---------------------------------------------------------------------------
std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) const {
  // 快照点解析与 Get 完全一致（并含同样的理由）：默认取 last_sequence_，
  // 用户显式传入的值截断到它。两条读路径的快照语义必须相同，否则"用同一个
  // ReadOptions 先迭代再点查"会看到两个不同的视图。
  SequenceNumber snapshot = last_sequence_.load(std::memory_order_acquire);
  if (options.snapshot < snapshot) snapshot = options.snapshot;

  // Version：与 Get 同样在锁内取引用。迭代期间 flush 可以换版本，但本迭代器
  // 只依赖这一份 Version 的文件列表（Table 本身由 VersionSet 的缓存持有），
  // 因此中途出现的新 SSTable 不会被看到——这与"快照读"语义一致。
  Version* v = versions_->current();
  if (v == nullptr) return std::make_unique<DBIterator>(&icmp_, snapshot);

  auto it = std::make_unique<DBIterator>(&icmp_, snapshot);

  // MemTable 作为第一个数据源。适配器在构造时 Ref、析构时 Unref，所以即使
  // 迭代途中发生 flush 把这个 MemTable 换出去，它也一定存活到迭代器销毁。
  //
  // 【为什么必须在锁内完成 Ref，而不能靠适配器构造时 Ref】
  // mem_ 恒有 DB 持有的所有权引用，所以"临界区内 load 到的表必然存活"——
  // 但这个保证只在**锁内**成立。锁一放，flush 就可能 store(new_mem) 并 Unref
  // 掉旧表；若旧表引用归零即被 delete，随后适配器构造函数对已释放内存调 Ref，
  // 就是 use-after-free。与 Get 的做法保持一致，不给两条读路径留差异。
  MemTable* m = nullptr;
  {
    std::lock_guard<std::mutex> lk(mem_mutex_);
    m = mem_.load(std::memory_order_relaxed);
    if (m != nullptr) m->Ref();
  }
  if (m != nullptr) {
    it->AddChild(std::make_unique<MemTableIteratorAdapter>(m));
  }

  // SSTable：从新到旧全部加入。L0 文件之间 key 范围可能重叠，交由归并处理，
  // 因此这里不需要（也不能）按范围裁剪——遍历就该看到全量数据。
  std::vector<uint64_t> borrowed;
  for (int i = static_cast<int>(v->files().size()) - 1; i >= 0; --i) {
    Table* t = versions_->AcquireTable(v->files()[i].number);
    if (t == nullptr) continue;  // 打不开的文件跳过，与 Get 的处理一致
    borrowed.push_back(v->files()[i].number);
    it->AddChild(t->NewIterator());
  }
  v->Unref();

  // 包一层，让这些引用跟着迭代器的生命周期走（见 TableReleasingIterator 注释）。
  //
  // 注意这里**不**自动 SeekToFirst：迭代器的初始位置是"无效"，由调用方显式
  // 选择 SeekToFirst / SeekToLast / Seek 之一。这样职责清晰，也避免了
  // "构造即定位"给后续想加反向遍历埋下语义歧义。
  return std::make_unique<TableReleasingIterator>(std::move(it), versions_,
                                                  std::move(borrowed));
}

}  // namespace tinystore
