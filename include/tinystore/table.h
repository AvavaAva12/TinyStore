#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "tinystore/block.h"
#include "tinystore/env.h"
#include "tinystore/filter_policy.h"
#include "tinystore/internal_key.h"
#include "tinystore/iterator.h"
#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {

namespace detail {
// Table 的迭代器实现只服务于 table.cpp，这里前置声明仅为满足友元声明。
class TableIterator;
}  // namespace detail

// ===========================================================================
// SSTable（Sorted String Table）—— 不可变的有序文件，LSM 树的持久化层
// ===========================================================================
//
// 文件布局（从上到下）：
//   [ data block 0 ]            ← 一段有序的 (internal_key, value)
//   [ data block 1 ]
//   ...
//   [ filter block ]            ← 布隆过滤器（可选）
//   [ metaindex block ]         ← "filter.<policy>" -> filter block 的句柄
//   [ index block ]             ← 每个 data block 一个分隔键 -> data block 句柄
//   [ footer ]                  ← metaindex 句柄 + index 句柄 + magic
//
// 读路径（Get）：读 footer 定位 index -> 二分找到候选 data block 句柄 ->
// pread 该 data block -> 块内二分找到 key。全程只随机读需要的那一个块。
class BlockHandle {
public:
  uint64_t offset = 0;
  uint64_t size = 0;

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);  // 原地消耗 input 头部的两个 varint64
};

struct Footer {
  BlockHandle meta_index_handle;
  BlockHandle index_handle;

  void EncodeTo(std::string* dst) const;
  static Status DecodeFrom(const Slice& input, Footer* footer);
};

// 块固定 48 字节：两个 BlockHandle（各至多 20B）+ 补齐 + 8B magic
constexpr size_t kTableMagicNumberSize = 8;
constexpr uint64_t kTableMagic = 0xdb4775248b80fb57ull;
constexpr size_t kFooterSize = 2 * (10 + 10) + kTableMagicNumberSize;  // = 48

// ---------------------------------------------------------------------------
// TableBuilder —— 把有序的 (internal_key, value) 流写成一个 SSTable
// ---------------------------------------------------------------------------
class TableBuilder {
public:
  // block_size：单个 data block 的目标大小（约值，超出即切下一块）。
  TableBuilder(const InternalKeyComparator* icmp, WritableFile* file,
               const FilterPolicy* filter_policy, size_t block_size);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // 要求 key 严格递增（调用方保证，例如按 internal_key 有序的 MemTable 迭代器）。
  void Add(const Slice& key, const Slice& value);

  // 写完所有剩余块 + footer。调用后文件内容完整（但调用方负责最后 Sync/Close）。
  Status Finish();

  // 已写入文件的字节数（含 footer）。
  uint64_t FileSize() const { return offset_; }

  // 该文件覆盖的 key 区间（最小 / 最大 internal_key），供 Version 元数据使用。
  const std::string& SmallestKey() const { return smallest_key_; }
  const std::string& LargestKey() const { return largest_key_; }

  // 已加入的 entry 条数。Compaction 用它判断"归并结果是否为空"——
  // 全部键都被删除时输出文件应当直接丢弃，而不是留下一个只有元数据的空表。
  size_t NumEntries() const { return num_entries_; }

private:
  // 把当前 data block 落盘 + 记录待定索引键。
  // 返回值语义与 WriteBlock 一致：失败时同时把错误记入 status_，
  // 使整个 TableBuilder 进入"作废"状态（后续 Add/Finish 均不再产生任何写入）。
  Status Flush();
  Status WriteBlock(const Slice& block, BlockHandle* handle);  // 加 trailer 后写入
  Status WriteRawBlock(const Slice& contents, BlockHandle* handle);

  const InternalKeyComparator* icmp_;
  WritableFile* file_;
  const FilterPolicy* filter_policy_;
  size_t block_size_;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;            // 上一个加入的 key（用于生成索引分隔键）
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;      // 上一个已落盘 data block 的句柄
  std::string smallest_key_;
  std::string largest_key_;
  size_t num_entries_ = 0;

  // 整文件布隆过滤器：收集所有 user_key，Finish 时一次性生成。
  std::vector<std::string> filter_keys_;

  uint64_t offset_ = 0;  // 已写字节数
  Status status_;
};

// ---------------------------------------------------------------------------
// Table —— SSTable 的只读视图
// ---------------------------------------------------------------------------
class Table {
public:
  // file 的所有权转移给 Table（析构时关闭）。file_size 为文件总字节数。
  static Status Open(const InternalKeyComparator* icmp,
                     std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                     Table** table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  ~Table();

  // 快照读：在文件内查找 user_key 在 snapshot 时刻的可见版本。
  //   OK + 填充 *value  —— 找到（最新可见的是有效值）
  //   NotFound          —— 文件内该 key 在 snapshot 下不存在 / 最新可见是删除
  // found 语义：*found == true 表示"文件内确有该 key 在 snapshot 下的版本"
  //             （值或删除）；*found == false 表示"文件内无相关版本"，调用方
  //             应继续查更老的文件。
  Status Get(const Slice& user_key, SequenceNumber snapshot,
             const FilterPolicy* filter_policy, std::string* value, bool* found) const;

  // 按 internal_key 升序遍历整个文件的全部 entry。
  //
  // 【它不做 MVCC 过滤】
  // 返回的是文件的**原始内容**：同一个 user_key 的多个历史版本都会依次出现，
  // 删除墓碑也会出现。snapshot 过滤与墓碑跳过是上层的职责（见 db_iterator），
  // 这样 SSTable 层可以保持"所见即文件所存"的简单语义，也便于独立测试。
  //
  // 【调用方约定】迭代期间 Table 必须存活（迭代器持有裸指针）。
  // data block 按需 pread，只有当前所在的那一个 block 占用内存。
  std::unique_ptr<Iterator> NewIterator() const;

  uint64_t ApproximateOffsetOf(const Slice& key) const;  // 供 Compaction 估算（W5）

  // 文件总字节数。TableCache 按它计费做 LRU 淘汰（W8），所以要对外暴露——
  // 常驻的索引块与过滤器只是其中一小部分，把它当作"这个缓存条目的成本"，
  // 数量级上足以反映真实的内存占用。
  uint64_t FileSize() const { return file_size_; }

private:
  friend class detail::TableIterator;

  Table(const InternalKeyComparator* icmp, std::unique_ptr<RandomAccessFile> file,
        uint64_t file_size);

  Status ReadBlock(const BlockHandle& handle, std::string* contents) const;

  const InternalKeyComparator* icmp_;
  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_size_;

  BlockHandle meta_index_handle_;
  BlockHandle index_handle_;

  std::string index_contents_;   // 常驻内存的索引块
  std::unique_ptr<Block> index_block_;
  std::string filter_contents_;  // 常驻内存的过滤器
};

}  // namespace tinystore
