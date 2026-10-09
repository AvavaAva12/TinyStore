#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "tinystore/env.h"
#include "tinystore/internal_key.h"
#include "tinystore/status.h"
#include "tinystore/table.h"

namespace tinystore {
namespace log { class Writer; }

// ===========================================================================
// 版本管理（Version / VersionSet / MANIFEST）
// ===========================================================================
//
// 【为什么需要它】
// W3 的 MemTable 是唯一数据归宿，进程退出即丢。W4 把 MemTable 周期性 flush 成
// 不可变的 SSTable 文件。于是"数据库当前状态"不再是单个结构，而是一组 SSTable
// 文件的集合——这就是 Version。每次 flush 都产生一个新 Version（多一个文件）。
//
// Version 用引用计数管理生命周期：查询线程在读 Version 时 Ref，读完 Unref。
// flush 线程生成一个新 Version 并原子地替换掉当前 Version，旧 Version 在最后一个
// 读者 Unref 后才真正销毁。这样**读不被 flush 阻塞**，且正在被读的旧 SSTable 不会
// 在被读期间被销毁（W1 锚点 6/7 在此落地）。
//
// MANIFEST 是 Version 的持久化日志：每次版本变更追加一条 VersionEdit 记录。
// 崩溃后重放 MANIFEST 即可重建 Version（无需扫描目录里所有 SSTable）。它还记录
// "当前 WAL 编号"，使得崩溃恢复只需重放那一个 WAL。
namespace Filename {

// 生成文件名：<dbname>/<6位零填充编号>.<suffix>，如 000123.log / 000124.ldb
std::string MakeFileName(const std::string& dbname, uint64_t number,
                          const char* suffix);
std::string ManifestFileName(const std::string& dbname);

// 解析文件名：<编号>.<后缀> -> 取前导数字部分。成功返回 true。
bool ParseFileName(const std::string& name, uint64_t* number);

}  // namespace Filename

// 单个 SSTable 的元数据（不持有文件内容，只描述它在版本中的位置）
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // 最小 internal_key（含 8 字节后缀）
  std::string largest;   // 最大 internal_key
};

// 一次版本变更：要么新增文件，要么删除文件（W4 只有新增，删除留给 W5 的 Compaction）
struct VersionEdit {
  std::string comparator;
  bool has_comparator = false;
  uint64_t log_number = 0;
  bool has_log_number = false;
  // 已提交的最大 sequence number：flush 把数据落进 SSTable 后，WAL 可能已被滚动成
  // 空文件，重放 WAL 会丢失 sequence。把它写进 MANIFEST，崩溃恢复才能还原出正确的
  // 快照点（否则快照=0，所有已 flush 的旧版本都会因 seq>0 而不可见）。
  SequenceNumber sequence = 0;
  bool has_sequence = false;
  std::vector<FileMetaData> new_files;  // 新增的 SSTable
  std::vector<uint64_t> deleted_files;  // 删除的文件编号

  void Clear();
  Status EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);
};

// 数据库某一时刻的快照：一组 SSTable 文件。引用计数、不可变。
class Version {
public:
  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }

  void Unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }

  // 文件按编号升序（也是"越新越靠后"）：Get 需从后往前扫描。
  const std::vector<FileMetaData>& files() const { return files_; }

private:
  friend class VersionSet;

  explicit Version(const std::vector<FileMetaData>& files) : files_(files) {}
  ~Version() = default;

  std::vector<FileMetaData> files_;
  mutable std::atomic<int> refs_{0};
};

class VersionSet {
public:
  VersionSet(const std::string& dbname, Env* env,
             const InternalKeyComparator* icmp);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // 分配一个新的文件编号（WAL 与 SSTable 共用一个计数器，保证全局唯一）。
  uint64_t NewFileNumber();

  // 当前 WAL 编号（MANIFEST 中记录）。0 表示还没有 WAL。
  uint64_t log_number() const { return log_number_; }

  // 已提交的最大 sequence number（从 MANIFEST 恢复得到），供 DB 设定快照点。
  SequenceNumber LastSequence() const { return last_sequence_; }

  // 当前活跃 Version（返回时已加好引用，调用方用完必须 Unref）。
  //
  // 【为什么这里必须加锁，而不能"load 指针 + TryRef 重试"】
  // 曾经的写法是：先 load 拿裸指针，再 TryRef 加引用，失败就重新 load。
  // 这个写法在原理上就是错的——TryRef 失败恰恰意味着"引用计数已归零、
  // 对象可能正在被 delete"，而一旦 delete 完成，裸指针已悬垂，
  // 此时无论重试多少次 load 都救不回来：读者已经在一个已释放对象上
  // 调用了 TryRef，这就是 use-after-free。
  //
  // 正确性来自一条不变量：**VersionSet 始终持有 current_ 的所有权引用**
  // （构造 / 换版本时都 Ref 过一次），因此"当前 Version"在运行期
  // 永远不会归零、永远不会被 delete。把「load + Ref」整体放进写者
  // 换版本所用的同一把锁，读写两侧就再无交错的可能：
  //   读者：锁内 load + Ref        （此刻 current_ 必然存活，Ref 安全）
  //   写者：锁内 exchange + Unref   （旧版本的最后一次 Unref 也在锁内）
  //
  // 临界区里只有指针获取与引用计数几条指令；真正耗时的读 SSTable、
  // 跳表查找都在锁外，不影响读路径的并发度。
  Version* current() const {
    std::lock_guard<std::mutex> lk(version_mutex_);
    Version* v = current_.load(std::memory_order_relaxed);
    if (v != nullptr) v->Ref();  // 返回前即持有引用，调用方读到的一定是活对象
    return v;
  }

  // 重放 MANIFEST 重建版本；通过出参返回"存活的 SSTable 编号"与"当前 WAL 编号"，
  // 供 DBImpl 清理孤儿文件、决定重放哪个 WAL。
  Status Recover(std::set<uint64_t>* live_files);

  // 把一次版本变更落盘到 MANIFEST，并原子安装成新的当前 Version。
  // 这是 flush 的"提交点"：调用返回后，新文件 + 新 WAL 编号对恢复逻辑可见。
  Status LogAndApply(VersionEdit* edit);

  // 收集当前 Version 引用的所有 SSTable 编号（供清理孤儿 .ldb 文件）。
  void AddLiveFiles(std::set<uint64_t>* live);

  // 按编号取得已打开的 Table（懒加载并缓存；W4 不淘汰，随 VersionSet 销毁）。
  // 返回的 Table* 在 VersionSet 生命周期内有效，且读路径全程只读、线程安全。
  Table* GetTable(uint64_t number);

private:
  Status WriteManifestRecord(const VersionEdit& edit);
  Status OpenManifestForAppend(uint64_t manifest_size);

  std::string dbname_;
  Env* env_;
  const InternalKeyComparator* icmp_;
  std::atomic<uint64_t> next_file_number_{0};

  std::string manifest_name_;
  WritableFile* manifest_file_ = nullptr;  // 不拥有 Env 句柄？拥有：析构时 Close
  log::Writer* manifest_writer_ = nullptr;

  uint64_t log_number_ = 0;

  SequenceNumber last_sequence_ = 0;  // 从 MANIFEST 的 sequence 记录恢复

  // 保护 current_ 的"读取 + 取引用"与"换版本 + 释放旧版本"，
  // 使读者不可能拿到一个正被 flush 释放的 Version（详见 current() 的注释）。
  mutable std::mutex version_mutex_;

  std::atomic<Version*> current_{nullptr};

  // Table 缓存：编号 -> 已打开的 Table
  mutable std::mutex cache_mutex_;
  std::map<uint64_t, Table*> table_cache_;
};

}  // namespace tinystore
