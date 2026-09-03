#include "tinystore/db_impl.h"

namespace tinystore {

// ===========================================================================
// DBImpl 实现
// ===========================================================================

DBImpl::DBImpl(const Options& options, const std::string& name)
    : env_(options.env),
      user_comparator_(options.comparator),
      icmp_(user_comparator_),
      dbname_(name),
      wal_name_(name + "/000001.log") {}

DBImpl::~DBImpl() {
  // 关闭 WAL：兜底再 Sync 一次（其实每次 Write 已经 fsync），然后 Close。
  // 注意 MemTable 是纯内存，进程退出即丢；真正的持久化靠 WAL，所以这里
  // 只要保证 WAL 落盘即可，MemTable 内容由 WAL 在下次 Open 时重放恢复。
  if (logfile_ != nullptr) {
    logfile_->Sync();
    logfile_->Close();
    delete logfile_;
  }
  delete log_;
  if (MemTable* m = mem_.load(std::memory_order_relaxed)) {
    m->Unref();  // 释放 DB 持有的引用
  }
}

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;

  if (options.error_if_exists) {
    // 目录已存在则报错（先确认是目录，避免同名普通文件被误判）
    if (options.env->FileExists(name)) {
      return Status::InvalidArgument(name, "exists");
    }
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
// Recover：重建 MemTable + 新建可写 WAL
// ---------------------------------------------------------------------------
Status DBImpl::Recover() {
  // 1) 确保目录存在
  Status s = env_->CreateDirIfMissing(dbname_);
  if (!s.ok()) return s;

  // 2) 先建好活跃 MemTable，并持有 DB 的那一份引用（refs_ 从 0 -> 1）
  MemTable* m = new MemTable(&icmp_);
  m->Ref();

  // 3) 若旧 WAL 存在，逐条 record 回放到 MemTable，并恢复 last_sequence_
  if (env_->FileExists(wal_name_)) {
    std::unique_ptr<SequentialFile> file;
    s = env_->NewSequentialFile(wal_name_, &file);
    if (!s.ok()) {
      m->Unref();
      return s;
    }
    log::Reader reader(file.get(), nullptr /*reporter*/, true /*checksum*/);
    std::string scratch;
    Slice record;
    SequenceNumber max_seq = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      WriteBatch batch;
      batch.SetContents(record);  // record 本身就是一条 WriteBatch 字节串
      const SequenceNumber first = WriteBatchInternal::Sequence(&batch);
      const uint32_t n = batch.Count();
      if (n > 0) {
        WriteBatchInternal::InsertInto(&batch, m);
        const SequenceNumber last = first + n - 1;
        if (last > max_seq) max_seq = last;
      }
    }
    last_sequence_.store(max_seq, std::memory_order_relaxed);
  }

  // 发布 MemTable（release：读者 acquire 读到 mem_ 时，必能看到里面的内容）
  mem_.store(m, std::memory_order_release);

  // 4) 新建可写 WAL（O_TRUNC 截断旧文件：其内容已恢复到内存）
  std::unique_ptr<WritableFile> wfile;
  s = env_->NewWritableFile(wal_name_, &wfile);
  if (!s.ok()) {
    m->Unref();
    return s;
  }
  logfile_ = wfile.release();
  log_ = new log::Writer(logfile_);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Write：Group Commit 的核心
// ---------------------------------------------------------------------------
Status DBImpl::Write(const WriteBatch& my_batch) {
  Writer w;
  w.batch = &my_batch;
  w.done = false;

  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&w);

  // 若我不是队首，说明已经有 leader 在提交：安静等它唤醒即可，
  // leader 会把我的 batch 合并进同一组一起提交。
  while (!w.done && &w != writers_.front()) {
    w.cv.wait(lock);
  }
  if (w.done) {
    return w.status;  // leader 已经把本请求提交好了
  }

  // ---- 我是 leader：把队列里所有等待者合并成一个大 batch ----
  WriteBatch combined;
  for (Writer* it : writers_) {
    if (it->batch != nullptr && it->batch->Count() > 0) {
      WriteBatchInternal::Append(&combined, it->batch);
    }
  }

  if (combined.Count() == 0) {
    // 整组都是空 batch：无需碰 WAL，直接唤醒所有人返回 OK。
    while (!writers_.empty()) {
      Writer* ready = writers_.front();
      writers_.pop_front();
      ready->done = true;
      ready->cv.notify_one();
    }
    return Status::OK();
  }

  // 分配本组起始 sequence，写进 WAL。整组只 fsync 一次。
  const SequenceNumber seq = last_sequence_.load(std::memory_order_relaxed) + 1;
  WriteBatchInternal::SetSequence(&combined, seq);

  Status s = log_->AddRecord(combined.Contents());
  if (s.ok()) s = logfile_->Sync();  // Group Commit 关键：一次 fsync 摊薄整组

  if (s.ok()) {
    MemTable* m = mem_.load(std::memory_order_acquire);
    WriteBatchInternal::InsertInto(&combined, m);
    // 先回放内存，再 release 发布新的 last_sequence_：
    // 读者 acquire 读到新 sequence 时，一定也能看到对应的内存数据。
    last_sequence_.store(seq + combined.Count() - 1,
                         std::memory_order_release);
  }

  // 唤醒同组所有 follower（同组共享提交结果）
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
// Get：无锁快照读
// ---------------------------------------------------------------------------
Status DBImpl::Get(const Slice& key, std::string* value) {
  MemTable* m = mem_.load(std::memory_order_acquire);
  if (m == nullptr) {
    return Status::NotFound("db not ready");
  }
  // 读期间 Ref，防止（未来 W4 flush 换表时）MemTable 被销毁。
  m->Ref();
  // acquire 读取 last_sequence_：与 Write 的 release 发布构成 happens-before，
  // 保证此时 MemTable 中 sequence <= snapshot 的数据都已可见。
  const SequenceNumber snapshot = last_sequence_.load(std::memory_order_acquire);
  Status s = m->Get(key, snapshot, value);
  m->Unref();
  return s;
}

}  // namespace tinystore
