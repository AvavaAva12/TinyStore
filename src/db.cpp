#include "tinystore/db_impl.h"

#include <algorithm>
#include <set>
#include <vector>

#include "tinystore/internal_key.h"
#include "tinystore/table.h"
#include "tinystore/version_set.h"

namespace tinystore {

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
    Table* table = versions_->GetTable(f.number);
    // GetTable 打不开时只能跳过：它的接口没有 Status 出口，
    // 无法区分"文件确实不在（孤儿清理后合法缺失）"与"文件损坏"。
    // 要真正区分，需要 W5 把 GetTable 改成返回 Status。
    if (table == nullptr) continue;
    bool tfound = false;
    Status ts = table->Get(key, snapshot, fp, value, &tfound);
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

}  // namespace tinystore
