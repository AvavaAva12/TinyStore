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
        new_files.push_back(f);
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
                       const InternalKeyComparator* icmp)
    : dbname_(dbname), env_(env), icmp_(icmp) {
  manifest_name_ = Filename::ManifestFileName(dbname_);
  // current_ 持有初始版本的一个所有权引用（refs_=1），这样 current()->TryRef()
  // 才能从 1 累加成功；否则 refs_==0 会让 TryRef 永远返回 false、current() 死循环。
  Version* init = new Version({});
  init->Ref();
  current_.store(init, std::memory_order_relaxed);
}

VersionSet::~VersionSet() {
  if (Version* v = current_.load(std::memory_order_acquire)) v->Unref();
  for (auto& kv : table_cache_) delete kv.second;
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
  Version* old = current_.exchange(v, std::memory_order_acq_rel);
  if (old) old->Unref();

  // 打开 MANIFEST 供后续追加（从当前文件尾续写）
  uint64_t msize = 0;
  env_->GetFileSize(manifest_name_, &msize);
  return OpenManifestForAppend(msize);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  // 调用方（DBImpl）负责把当前已提交的最大 sequence 填进 edit，随本条记录一起落盘，
  // 崩溃恢复才能还原快照点。这里只消费它，不能覆盖——VersionSet 自己的
  // last_sequence_ 只在 Recover 时被填充，运行期并不跟踪写入进度。
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
  Version* old = current_.exchange(nv, std::memory_order_acq_rel);
  if (old) old->Unref();
  return Status::OK();
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (const auto& f : current_.load(std::memory_order_acquire)->files()) live->insert(f.number);
}

Table* VersionSet::GetTable(uint64_t number) {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  auto it = table_cache_.find(number);
  if (it != table_cache_.end()) return it->second;

  const std::string fname = Filename::MakeFileName(dbname_, number, "ldb");
  std::unique_ptr<RandomAccessFile> file;
  if (!env_->NewRandomAccessFile(fname, &file).ok()) return nullptr;
  uint64_t size = 0;
  if (!env_->GetFileSize(fname, &size).ok()) return nullptr;

  Table* table = nullptr;
  if (!Table::Open(icmp_, std::move(file), size, &table).ok()) return nullptr;
  table_cache_[number] = table;
  return table;
}

}  // namespace tinystore
