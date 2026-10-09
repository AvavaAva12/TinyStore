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
  void Seek(const Slice& target) override { inner_->Seek(target); }
  void Next() override { inner_->Next(); }
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
  versions_ = new VersionSet(dbname_, env_, &icmp_);
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
}

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;

  if (options.error_if_exists && options.env->FileExists(name)) {
    return Status::InvalidArgument(name, "exists");
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
        WriteBatchInternal::InsertInto(&batch, m);
        max_seq = std::max(max_seq, first + n - 1);
      }
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

int DBImpl::PickCompactionLevel(const Version* v) const {
  if (v->NumLevelFiles(0) >= options_.l0_compaction_trigger) return 0;
  for (int lvl = 1; lvl < options_.max_num_levels; ++lvl) {
    if (v->LevelBytes(lvl) > LevelCapacity(lvl)) return lvl;
  }
  return -1;
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
  const int next_level = level + 1;
  const bool next_is_final = (next_level >= options_.max_num_levels - 1);

  // 1) 选源文件。L0 整体参与；更高层只挑一个文件（挑最小的那个最划算，
  //    因为它归并后能最快沉到下一层）。
  std::vector<const FileMetaData*> inputs;
  {
    Version* v = versions_->current();
    if (v->files().empty()) {
      v->Unref();
      return Status::OK();
    }
    for (const auto* f : v->FilesAtLevel(level)) inputs.push_back(f);
    v->Unref();
  }
  if (inputs.empty()) return Status::OK();
  if (level > 0 && inputs.size() > 1) {
    const FileMetaData* smallest =
        *std::min_element(inputs.begin(), inputs.end(),
                          [](const FileMetaData* a, const FileMetaData* b) {
                            return a->number < b->number;
                          });
    inputs.assign(1, smallest);
  }

  // 2) 选下一层中与源文件范围重叠的文件。这些文件要被源文件的新版本覆盖，
  //    所以必须一起参与归并，否则旧版本会"复活"。
  std::vector<uint64_t> input_numbers;
  std::vector<const FileMetaData*> inputs_next;
  {
    Version* v = versions_->current();
    for (const auto* f : inputs) input_numbers.push_back(f->number);
    for (const auto* g : v->FilesAtLevel(next_level)) {
      bool overlaps = false;
      for (const auto* f : inputs) {
        if (RangesOverlap(&icmp_, *f, *g)) {
          overlaps = true;
          break;
        }
      }
      if (overlaps) {
        inputs_next.push_back(g);
        input_numbers.push_back(g->number);
      }
    }
    v->Unref();
  }

  // 3) 归并。用 DBIterator 串起所有输入源：它按 internal_key 有序归并，并自动
  //    过滤掉被更新的旧版本与墓碑，输出的正是"该文件最终形态"的内容。
  //    snapshot 用 kMaxSequenceNumber 表示"要全部可见"，因为这里要的是
  //    把所有版本压平成最新状态，而不是某个时刻的视图。
  std::vector<FileMetaData> outputs;
  {
    // borrow 先声明、merged 后声明 => 析构时 merged 先走、borrow 再归还引用。
    TableBorrow borrow(versions_);
    DBIterator merged(&icmp_, kMaxSequenceNumber);
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
    {
      Version* cur = versions_->current();
      if (cur != nullptr) {
        trigger = PickCompactionLevel(cur);
        cur->Unref();
      }
    }
    if (trigger < 0) break;
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

  if (s.ok()) {
    MemTable* m = mem_.load(std::memory_order_acquire);
    WriteBatchInternal::InsertInto(&combined, m);
    // 先回放内存，再 release 发布新的 last_sequence_
    last_sequence_.store(seq + combined.Count() - 1, std::memory_order_release);

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

  const SequenceNumber snapshot = last_sequence_.load(std::memory_order_acquire);
  bool found = false;
  Status s = m->Get(key, snapshot, value, &found);
  if (found) {  // 命中（值或删除）-> 不必再查 SSTable
    m->Unref();
    return s;
  }
  // 未命中：后续只查 SSTable，不再需要 MemTable 的引用，提前释放。
  m->Unref();

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
  const SequenceNumber snapshot = options.snapshot;

  // Version：与 Get 同样在锁内取引用。迭代期间 flush 可以换版本，但本迭代器
  // 只依赖这一份 Version 的文件列表（Table 本身由 VersionSet 的缓存持有），
  // 因此中途出现的新 SSTable 不会被看到——这与"快照读"语义一致。
  Version* v = versions_->current();
  if (v == nullptr) return std::make_unique<DBIterator>(&icmp_, snapshot);

  auto it = std::make_unique<DBIterator>(&icmp_, snapshot);

  // MemTable 作为第一个数据源。适配器在构造时 Ref、析构时 Unref，所以即使
  // 迭代途中发生 flush 把这个 MemTable 换出去，它也一定存活到迭代器销毁。
  MemTable* m = nullptr;
  {
    std::lock_guard<std::mutex> lk(mem_mutex_);
    m = mem_.load(std::memory_order_relaxed);
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
  return std::make_unique<TableReleasingIterator>(std::move(it), versions_,
                                                  std::move(borrowed));

  it->SeekToFirst();
  return it;
}

}  // namespace tinystore
