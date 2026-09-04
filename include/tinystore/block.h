#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tinystore/comparator.h"
#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {

// ===========================================================================
// Block —— SSTable 的"块"：数据块 / 索引块 / 元数据块共用同一格式
// ===========================================================================
//
// 【为什么要分块】
// SSTable 可能很大（GB 级）。如果整体读进内存，既不现实也没必要——一次 Get
// 只需其中一个数据块。把文件切成定长（约 block_size）的块，读时按索引定位、
// 只把相关的一个块 pread 进内存，内存占用与 IO 都降到 O(1)。
//
// 【块内格式（详见 block.cc）】
//   entries:  每条 = [shared(varint32)][non_shared(varint32)][value_len(varint32)]
//                       [key 的非共享字节][value 字节]
//   restart:  重启点数组（uint32 小端）+ 重启点个数（uint32 小端）
//   trailer:  crc32c(4B) + type(1B)   ← 校验和，防静默损坏
//
// 每个 entry 存的是"与上一条 key 的共享前缀长度"（前缀压缩），所以块里只存
// key 的增量；重启点是"完整 key"的位置，用于二分查找后线性回溯，避免解压整条链。
//
// Block 是只读、不可变的：一旦从磁盘读入，所有读取都是无锁的（pread + 只读内存），
// 天然可被多个查询线程并发访问，不需要任何互斥（这正是 W3"无锁读"精神的延续）。
class BlockBuilder {
public:
  explicit BlockBuilder(int block_restart_interval = 16);

  void Reset();

  // 要求 key 严格大于之前加入的所有 key（调用方保证有序，例如 MemTable 迭代器）。
  void Add(const Slice& key, const Slice& value);

  // 生成完整块内容（含重启点数组）。返回内容在下次 Add/Reset 前保持稳定。
  Slice Finish();

  // 当前已占用字节数（不含 Finish 后才追加的重启点数组）。
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

private:
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;            // 距上一个重启点的 entry 数
  bool finished_ = false;
  std::string last_key_;
  int block_restart_interval_;
};

// ---------------------------------------------------------------------------
// Block 只读解析。
// ---------------------------------------------------------------------------
class Block {
public:
  // contents 必须活过 Block 的使用期（Table 持有对应的缓冲区）。内容包含 5 字节
  // trailer（crc + type）。
  explicit Block(const Slice& contents);

  // 找到第一个 key >= lookup 的 entry。
  //   OK + 填充 *key/*value  —— 命中，key 是该 entry 的完整 key
  //   NotFound               —— 没有任何 entry >= lookup
  Status Get(const Comparator* cmp, const Slice& lookup,
             std::string* key, std::string* value) const;

  // 只读迭代器（供 Table 的索引定位 / 数据块定位使用）。
  class Iter {
  public:
    explicit Iter(const Block* block, const Comparator* cmp);

    void SeekToFirst();
    void Seek(const Slice& target);   // 第一个 key >= target
    void Next();
    bool Valid() const { return valid_; }
    Slice key() const { return Slice(key_); }
    Slice value() const { return Slice(value_); }

  private:
    // 解析位于 offset 的 entry，prev_key 为前一条完整 key（用于前缀还原）。
    // 成功时填充 *out_key/*out_value，并把 *next_offset 指向本条之后的偏移。
    bool ParseEntry(uint32_t offset, const std::string& prev_key,
                    std::string* out_key, std::string* out_value,
                    uint32_t* next_offset);

    const Block* block_;
    const Comparator* cmp_;
    uint32_t current_ = 0;     // 当前 entry 在块内的起始偏移
    uint32_t next_offset_ = 0; // 当前 entry 之后的偏移
    uint32_t restart_index_ = 0;
    bool valid_ = false;
    std::string key_;
    std::string value_;
  };

  uint32_t NumRestarts() const { return num_restarts_; }

private:
  friend class Iter;

  const char* data_;
  size_t size_;
  uint32_t num_restarts_;
  const char* restarts_ptr_;   // 重启点数组起点
  uint32_t entries_end_;       // entry 区结束偏移（= 重启点数组起点）
};

}  // namespace tinystore
