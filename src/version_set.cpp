#include "tinystore/version_set.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "tinystore/coding.h"
#include "tinystore/internal_key.h"
#include "tinystore/log_reader.h"
#include "tinystore/log_writer.h"

namespace tinystore {
namespace Filename {

std::string MakeFileName(const std::string& dbname, uint64_t number,
                          const char* suffix) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%06llu", static_cast<unsigned long long>(number));
  return dbname + "/" + buf + "." + suffix;
}

std::string ManifestFileName(const std::string& dbname) {
  return dbname + "/MANIFEST";
}

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

bool ParseFileName(const std::string& name, uint64_t* number) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) return false;
  const std::string prefix = name.substr(0, dot);
  if (prefix.empty()) return false;
  for (char c : prefix) {
    if (c < '0' || c > '9') return false;
  }
  *number = std::strtoull(prefix.c_str(), nullptr, 10);
  return true;
}

}  // namespace Filename

// ===========================================================================
// VersionEdit
// ===========================================================================

void VersionEdit::Clear() {
  comparator.clear();
  has_comparator = false;
  log_number = 0;
  has_log_number = false;
  new_files.clear();
  deleted_files.clear();
}

Status VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator) {
    dst->push_back(1);
    PutLengthPrefixedSlice(dst, comparator);
  }
  if (has_log_number) {
    dst->push_back(2);
    PutVarint64(dst, log_number);
  }
  if (has_sequence) {
    dst->push_back(5);
    PutVarint64(dst, sequence);
  }
  for (const auto& f : new_files) {
    dst->push_back(3);
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, f.smallest);
    PutLengthPrefixedSlice(dst, f.largest);
    // 层号用独立的 tag 6 紧跟在该文件之后，而不是塞进 tag 3 的字段序列里。
    // 这样 W4 之前写下的旧 MANIFEST 记录（没有 tag 6）依然能正常解析，
    // level 取默认值 0 —— 也就是"旧数据都算 L0"，与当时的语义一致。
    // 若把 level 混进 tag 3，旧记录就会因为少一个字段而整体错位。
    dst->push_back(6);
    PutVarint64(dst, static_cast<uint64_t>(f.level));
  }
  for (uint64_t n : deleted_files) {
    dst->push_back(4);
    PutVarint64(dst, n);
  }
  return Status::OK();
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Slice in = src;
  while (!in.empty()) {
    const uint8_t tag = static_cast<uint8_t>(in[0]);
    in.remove_prefix(1);
    switch (tag) {
      case 1: {
        Slice s;
        if (!GetLengthPrefixedSlice(&in, &s)) return Status::Corruption("version edit");
        comparator = s.ToString();
        has_comparator = true;
        break;
      }
      case 2: {
        if (!GetVarint64(&in, &log_number)) return Status::Corruption("version edit");
        has_log_number = true;
        break;
      }
      case 3: {
        FileMetaData f;
        if (!GetVarint64(&in, &f.number)) return Status::Corruption("version edit");
        if (!GetVarint64(&in, &f.file_size)) return Status::Corruption("version edit");
        Slice a, b;
        if (!GetLengthPrefixedSlice(&in, &a)) return Status::Corruption("version edit");
        if (!GetLengthPrefixedSlice(&in, &b)) return Status::Corruption("version edit");
        f.smallest = a.ToString();
        f.largest = b.ToString();
        new_files.push_back(std::move(f));
        break;
      }
      case 6: {
        // 层号作用于"最近解析出来的那个文件"。若还没有文件，说明 MANIFEST
        // 已经损坏（tag 6 脱离了它的 tag 3），直接报错而不是静默忽略。
        uint64_t lvl = 0;
        if (!GetVarint64(&in, &lvl)) return Status::Corruption("version edit");
        if (new_files.empty()) return Status::Corruption("level tag without file");
        new_files.back().level = static_cast<int>(lvl);
        break;
      }
      case 4: {
        uint64_t n;
        if (!GetVarint64(&in, &n)) return Status::Corruption("version edit");
        deleted_files.push_back(n);
        break;
      }
      case 5: {
        if (!GetVarint64(&in, &sequence)) return Status::Corruption("version edit");
        has_sequence = true;
        break;
      }
      default:
        return Status::Corruption("unknown version edit tag");
    }
  }
  return Status::OK();
}

// ===========================================================================
// VersionSet
// ===========================================================================

VersionSet::VersionSet(const std::string& dbname, Env* env,
                       const InternalKeyComparator* icmp, uint64_t max_cache_bytes)
    : dbname_(dbname), env_(env), icmp_(icmp), max_cache_bytes_(max_cache_bytes) {
  manifest_name_ = Filename::ManifestFileName(dbname_);
  // current_ 持有初始版本的一个所有权引用（refs_=1）。这不是可选的优化，
  // 而是 current() 正确性的前提：只有"当前 Version 永不归零、永不被 delete"，
  // 读者才能在锁内安全 Ref 到一个确定存活的对象（详见 version_set.h 的 current()）。
  Version* init = new Version({});
  init->Ref();
  current_.store(init, std::memory_order_relaxed);
}

VersionSet::~VersionSet() {
  if (Version* v = current_.load(std::memory_order_acquire)) v->Unref();
  for (auto& kv : table_cache_) delete kv.second.table;
  if (manifest_file_ != nullptr) {
    manifest_file_->Close();
    delete manifest_file_;
  }
  delete manifest_writer_;
}

uint64_t VersionSet::NewFileNumber() {
  return next_file_number_.fetch_add(1, std::memory_order_relaxed) + 1;
}

Status VersionSet::WriteManifestRecord(const VersionEdit& edit) {
  std::string record;
  edit.EncodeTo(&record);
  Status s = manifest_writer_->AddRecord(record);
  if (s.ok()) s = manifest_file_->Sync();  // 版本变更必须落盘，否则崩溃即丢
  return s;
}

Status VersionSet::OpenManifestForAppend(uint64_t size) {
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewAppendableFile(manifest_name_, &file);
  if (!s.ok()) return s;
  manifest_file_ = file.release();
  manifest_writer_ = new log::Writer(manifest_file_, size % log::kBlockSize);
  return Status::OK();
}

Status VersionSet::Recover(std::set<uint64_t>* live_files) {
  if (!env_->FileExists(manifest_name_)) {
    // 全新库：创建空 MANIFEST 并写一条带 comparator 名的初始记录
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewAppendableFile(manifest_name_, &file);
    if (!s.ok()) return s;
    manifest_file_ = file.release();
    manifest_writer_ = new log::Writer(manifest_file_);
    VersionEdit edit;
    edit.comparator = icmp_->user_comparator()->Name();
    edit.has_comparator = true;
    s = WriteManifestRecord(edit);
    if (!s.ok()) return s;
    log_number_ = 0;
    return Status::OK();
  }

  // 已有 MANIFEST：重放记录重建版本
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(manifest_name_, &file);
  if (!s.ok()) return s;
  log::Reader reader(file.get(), nullptr, true);
  std::string scratch;
  Slice record;
  std::vector<FileMetaData> files;
  uint64_t log_num = 0;
  bool has_log = false;
  while (reader.ReadRecord(&record, &scratch)) {
    VersionEdit edit;
    if (!edit.DecodeFrom(record).ok()) {
      return Status::Corruption("corrupt manifest record");
    }
    for (const auto& f : edit.new_files) files.push_back(f);
    for (uint64_t n : edit.deleted_files) {
      files.erase(std::remove_if(files.begin(), files.end(),
                                 [n](const FileMetaData& x) { return x.number == n; }),
                  files.end());
    }
    if (edit.has_log_number) {
      log_num = edit.log_number;
      has_log = true;
    }
    if (edit.has_sequence) {
      last_sequence_ = std::max(last_sequence_, edit.sequence);
    }
  }
  log_number_ = has_log ? log_num : 0;

  // 文件编号计数器要跳过所有已存在编号（SSTable + 当前 WAL），
  // 否则新文件可能复用一个孤儿文件的编号，导致 GetTable 打开到陈旧数据。
  uint64_t max_num = log_num;
  for (const auto& f : files) max_num = std::max(max_num, f.number);
  next_file_number_.store(max_num, std::memory_order_relaxed);

  // 把当前版本引用的 SSTable 编号（以及当前 WAL 编号）回传给调用方，
  // 供其清理孤儿文件、决定崩溃后重放哪个 WAL。
  if (live_files != nullptr) {
    for (const auto& f : files) live_files->insert(f.number);
    if (has_log) live_files->insert(log_num);
  }

  Version* v = new Version(files);
  v->Ref();  // 接手 VersionSet 对 current_ 的所有权引用
  // 换版本与释放旧版本必须在 current() 的同一把锁内完成，否则读者可能
  // 已经 load 到 old 却还没来得及 Ref，old 就被这里 Unref 到 0 而 delete。
  {
    std::lock_guard<std::mutex> lk(version_mutex_);
    Version* old = current_.exchange(v, std::memory_order_acq_rel);
    if (old) old->Unref();
  }

  // 打开 MANIFEST 供后续追加（从当前文件尾续写）
  uint64_t msize = 0;
  env_->GetFileSize(manifest_name_, &msize);
  return OpenManifestForAppend(msize);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  // 串行化整个提交流程：后台 compaction 与前台 flush 可能并发调用。
  std::lock_guard<std::mutex> edit_lk(version_edit_mutex_);
  Status s = WriteManifestRecord(*edit);
  if (!s.ok()) return s;

  std::vector<FileMetaData> files = current_.load(std::memory_order_acquire)->files();
  for (const auto& f : edit->new_files) files.push_back(f);
  for (uint64_t n : edit->deleted_files) {
    files.erase(std::remove_if(files.begin(), files.end(),
                               [n](const FileMetaData& x) { return x.number == n; }),
                files.end());
  }
  if (edit->has_log_number) log_number_ = edit->log_number;
  if (edit->has_sequence) last_sequence_ = edit->sequence;

  Version* nv = new Version(files);
  nv->Ref();  // 接手 VersionSet 对 current_ 的所有权引用
  {
    std::lock_guard<std::mutex> lk(version_mutex_);
    Version* old = current_.exchange(nv, std::memory_order_acq_rel);
    if (old) old->Unref();
  }
  return Status::OK();
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (const auto& f : current_.load(std::memory_order_acquire)->files()) live->insert(f.number);
}

void VersionSet::OpenTableLocked(uint64_t number, Table** out) {
  *out = nullptr;
  const std::string fname = Filename::MakeFileName(dbname_, number, "ldb");
  std::unique_ptr<RandomAccessFile> file;
  if (!env_->NewRandomAccessFile(fname, &file).ok()) return;
  uint64_t size = 0;
  if (!env_->GetFileSize(fname, &size).ok()) return;
  if (!Table::Open(icmp_, std::move(file), size, out).ok()) *out = nullptr;
}

Table* VersionSet::AcquireTable(uint64_t number) {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  auto it = table_cache_.find(number);
  if (it == table_cache_.end()) {
    Table* t = nullptr;
    OpenTableLocked(number, &t);
    if (t == nullptr) return nullptr;
    TableEntry entry;
    entry.table = t;
    entry.size = t->FileSize();
    it = table_cache_.emplace(number, entry).first;
    cache_bytes_ += entry.size;
  }
  // 已标记淘汰的文件不再交给新读者：它的内容已经被 compaction 取代，
  // 新读者应该去读新的 Version 里的文件。
  if (it->second.evicted) return nullptr;
  ++it->second.refs;
  // 刷新访问时间。必须在 Ref 之后做：新读者确实要用它了。
  // 即使本次 AcquireTable 最终把条目淘汰掉，这里刷新的 last_used 也无妨——
  // 淘汰只在"超过容量上限且该条目无人使用时"发生，refs>0 会挡住它。
  it->second.last_used = ++cache_clock_;
  // 新条目可能让缓存超标，这里顺带做一次回收。放在 Ref 之后是为了让
  // 当前读者持有的表不会被本次调用自己淘汰掉。
  EvictLeastRecentlyUsedLocked();
  return it->second.table;
}

void VersionSet::EvictLeastRecentlyUsedLocked() {
  if (max_cache_bytes_ == 0) return;  // 未设上限，保持"只增不减"
  if (cache_bytes_ <= max_cache_bytes_) return;

  // 收集可淘汰条目，按 last_used 升序（最久未使用在前）。
  //
  // 【为什么用局部 vector 排序，而不是每次都扫全表】
  // 淘汰是 O(n log n) 但只发生在超限时；若把排序留在 map 的遍历里，
  // 每次 AcquireTable 都要付这个代价，而绝大多数时候根本没超限。
  // 先用 O(n) 收集 + 提前退出判断，超限时才付排序的钱。
  std::vector<std::pair<uint64_t, uint64_t>> candidates;  // (last_used, number)
  candidates.reserve(table_cache_.size());
  for (const auto& kv : table_cache_) {
    // 跳过正被读者使用的、以及已由 compaction 标记淘汰的条目。
    if (kv.second.refs > 0 || kv.second.evicted) continue;
    candidates.emplace_back(kv.second.last_used, kv.first);
  }
  std::sort(candidates.begin(), candidates.end());

  for (const auto& c : candidates) {
    if (cache_bytes_ <= max_cache_bytes_) break;
    auto it = table_cache_.find(c.second);
    if (it == table_cache_.end()) continue;
    // 复查：收集期间可能有并发 ReleaseTable 把 refs 降到 0（不影响），
    // 也可能有新的读者进来。锁内检查是权威判据。
    if (it->second.refs > 0 || it->second.evicted) continue;
    delete it->second.table;
    cache_bytes_ -= it->second.size;
    table_cache_.erase(it);
  }
}

void VersionSet::ReleaseTable(uint64_t number) {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  auto it = table_cache_.find(number);
  if (it == table_cache_.end()) return;
  if (--it->second.refs > 0) return;
  // 引用归零：只有被标记淘汰的才真正释放，其余留在缓存里复用。
  // 释放后条目变成"可被 LRU 回收"的候选——若此时缓存已超标，
  // 本次释放正好腾出的额度就顺手用掉。
  if (it->second.evicted) {
    delete it->second.table;
    cache_bytes_ -= it->second.size;
    table_cache_.erase(it);
    return;
  }
  EvictLeastRecentlyUsedLocked();
}

void VersionSet::EvictTable(uint64_t number) {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  auto it = table_cache_.find(number);
  if (it == table_cache_.end()) return;
  it->second.evicted = true;
  // 没有读者时才立即释放；否则等最后一个 ReleaseTable。
  // 直接 delete 会把并发读者手里的裸指针变成悬垂指针。
  if (it->second.refs <= 0) {
    delete it->second.table;
    cache_bytes_ -= it->second.size;
    table_cache_.erase(it);
  }
}

}  // namespace tinystore
