#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#include "tinystore/arena.h"
#include "tinystore/coding.h"
#include "tinystore/internal_key.h"
#include "tinystore/skiplist.h"
#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {

// ===========================================================================
// LookupKey —— MemTable 查找用的临时键
// ===========================================================================
//
// 跳表节点里存的是一条"记录缓冲区" [varint(ik_size)][internal_key][...]，
// 比较器通过 GetLengthPrefixedSlice 取出 internal_key 来比。为了让"查找目标"
// 也能被同一个比较器处理，查找键必须也是同样的布局，只是不需要 value 部分。
//
// LookupKey 把 (user_key, snapshot) 编码成：
//     [varint(ik_size)][user_key][packed(snapshot, kTypeValue)]
// 其中 packed = PackSequenceAndType(snapshot, kTypeValue)。
//
// 【为什么要塞一个 kTypeValue】
// InternalKeyComparator 对相同 user_key 按 packed 降序排。编码查找键时若用
// snapshot 作为 sequence，那么所有 sequence > snapshot 的版本（packed 更大）
// 都会排在查找键"前面"，FindGreaterOrEqual 会跳过它们，正好停在
// "sequence <= snapshot 的最新版本"上——这就是快照读的实现。tag 填 kTypeValue
// 还是 kTypeDeletion 不影响定位（同一 user_key 不可能有两个相同 sequence）。
class LookupKey {
public:
  LookupKey(const Slice& user_key, SequenceNumber seq) {
    const size_t usz = user_key.size();
    const size_t ik_size = usz + kInternalKeySuffixSize;
    rep_.resize(VarintLength(ik_size) + ik_size);
    char* p = rep_.data();
    char* q = EncodeVarint32(p, static_cast<uint32_t>(ik_size));
    if (usz > 0) std::memcpy(q, user_key.data(), usz);
    EncodeFixed64(q + usz, PackSequenceAndType(seq, kTypeValue));
  }

  const char* data() const { return rep_.data(); }
  size_t size() const { return rep_.size(); }

private:
  std::string rep_;
};

// ===========================================================================
// MemTable —— 内存中的可变数据表（写路径的核心）
// ===========================================================================
//
// 【职责】
//   1. 承接所有写入：Add 把 (seq, type, key, value) 编码成 internal_key，连同
//      value 一起放进 Arena 支撑的跳表里；
//   2. 承接快照读：Get 用 MVCC 语义返回某个 sequence 时刻的值；
//   3. 可被整体 flush 成 SSTable（W4）：通过一个迭代器按序吐出全部条目，
//      用完即弃——Arena 一次性释放所有节点。
//
// 【为什么节点与 value 分开存两段 arena 内存】
// 跳表是泛型容器，它只负责"按 key 有序地放指针"。因此记录缓冲区（internal_key
// + value）由 MemTable 在 Arena 里单独分配，再把它的指针交给跳表节点持有。
// 两段都是 Arena 分配，随 MemTable 一起回收，没有 per-node 的堆碎片。
//
// 【引用计数：为什么 MemTable 要共享】
// flush 线程正在把某个 MemTable 落盘的同时，查询线程可能还在读它。于是 MemTable
// 在"写入线程"和"flush / 查询线程"之间共享，用 Ref/Unref 管理生命周期：当引用
// 归零才 delete。一旦一个 MemTable 变成 immutable（不再被写入），它就没有写者，
// 多个读者并发读是安全的（跳表 next 是 atomic）。
class MemTable {
private:
  // 跳表比较器：从记录缓冲区取出 internal_key 交给 InternalKeyComparator。
  // 必须在 table_ / Iterator 之前完整定义——SkipList<...> 的成员需要它完整。
  struct MemTableKeyComparator {
    const InternalKeyComparator* icmp;
    int operator()(const char* a, const char* b) const {
      return icmp->Compare(GetLengthPrefixedSlice(a), GetLengthPrefixedSlice(b));
    }
  };

public:
  explicit MemTable(const InternalKeyComparator* comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }

  // 写入一条记录。seq 由调用方分配（全局单调递增），type 区分增/删。
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // 快照读：返回 user_key 在 snapshot 序号下的值。
  //   OK + 填充 *value   —— 找到快照可见的最新有效值
  //   NotFound           —— 该 key 不存在，或最新可见版本是删除（墓碑）
  //   Corruption         —— internal_key 无法解析（不应发生，除非内存损坏）
  Status Get(const Slice& user_key, SequenceNumber snapshot,
             std::string* value) const;

  // 近似内存占用（节点 + 记录，均由 Arena 统计）
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  // ---- 迭代器：用于 flush / 调试，按 user_key 升序 + sequence 降序遍历 ----
  class Iterator {
  public:
    explicit Iterator(const MemTable* mem);

    bool Valid() const;
    void SeekToFirst();
    void SeekToLast();
    void Next();
    void Prev();
    void Seek(const Slice& user_key);

    Slice user_key() const;
    Slice internal_key() const;
    Slice value() const;
    SequenceNumber sequence() const;
    ValueType type() const;

  private:
    typename SkipList<const char*, MemTableKeyComparator>::Iterator iter_;
    const MemTable* mem_;
  };

private:
  friend class Iterator;

  InternalKeyComparator const* const comparator_;
  Arena arena_;
  SkipList<const char*, MemTableKeyComparator> table_;
  std::atomic<int> refs_;
  mutable std::mutex mu_;  // W2：写与读都加锁（W3 放开为只锁写）
};

}  // namespace tinystore
