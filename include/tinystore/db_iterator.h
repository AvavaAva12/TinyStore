#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "tinystore/internal_key.h"
#include "tinystore/iterator.h"
#include "tinystore/memtable.h"

namespace tinystore {

// ===========================================================================
// DBIterator —— 跨 MemTable 与多个 SSTable 的归并迭代器
//
// 【它解决什么问题】
// 数据分散在多个各自有序、但彼此 key 范围可能重叠的来源里：
//   * 活跃 MemTable（W4 起写入的新数据，最"新"）
//   * 若干 SSTable（每次 flush 产生一个；因为 flush 边界随意，L0 文件之间的
//     key 范围是**重叠**的，不能简单按文件顺序拼接）
// Get 是"点查"，逐个文件问一句就够；但遍历要求把多个有序流合成**一条**
// 全局有序流，这正是 k 路归并的经典场景。
//
// 【分层过滤：为什么 MVCC 逻辑放在这一层】
// 下面每个子迭代器都吐出文件/内存表的**原始内容**——同一个 user_key 的多个
// 历史版本、删除墓碑，全都原样输出。过滤规则统一在 DBIterator 做：
//   1. 只看 sequence <= snapshot 的版本（历史读的基础）
//   2. 同一个 user_key 的多个版本，只输出其中最新的那个可见版本
//   3. 该版本是墓碑 -> 整个 user_key 不输出
// 这样 SSTable 层保持"所见即文件所存"的简单语义，能独立测试；
// MVCC 规则也只有一处实现，不会散落到多个数据源上各写一遍。
//
// 【墓碑遮挡老版本，是刻意的】
// 若某 user_key 在 snapshot 时刻的最新可见版本是墓碑，输出它更老的值就是
// 错误——那等于"删了又复活"。因此一旦命中墓碑，就跳过该 user_key 的
// **所有**版本（而不是跳过墓碑本身继续往下找）。
// ===========================================================================
class DBIterator : public Iterator {
public:
  DBIterator(const InternalKeyComparator* icmp, SequenceNumber snapshot)
      : icmp_(icmp), snapshot_(snapshot) {}

  DBIterator(const DBIterator&) = delete;
  DBIterator& operator=(const DBIterator&) = delete;
  ~DBIterator() override = default;

  // 追加一个数据源。**所有子迭代器的 key() 必须返回完整的 internal_key**
  //（含 8 字节后缀），归并与版本比较都依赖它。
  void AddChild(std::unique_ptr<Iterator> child) {
    children_.push_back(std::move(child));
  }

  // Iterator 接口
  bool Valid() const override { return valid_; }
  Slice key() const override { return Slice(cur_key_); }

  // 完整 internal key（user_key + 8 字节 seq/type 后缀）。
  // 只给 Compaction 用：归并时必须把版本号原样写进新 SSTable，否则快照读失效。
  Slice internal_key() const { return Slice(cur_internal_key_); }
  Slice value() const override { return Slice(cur_value_); }
  Status status() const override { return status_; }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    valid_ = false;
    FindNextVisibleEntry();
  }

  // 按 user_key 定位（对外语义，与 Get 一致）。
  //
  // 直接把 user_key 转发给各个子迭代器：它们会定位到该 key 的**最新**版本
  // （子层不知道 snapshot）。随后由 FindNextVisibleEntry 跳过所有 seq > snapshot
  // 的版本，最终停在"快照下最新的可见版本"。这样做的额外代价只是几轮循环，
  // 换来的是子迭代器不必知道 snapshot 的存在——它们的语义保持干净。
  void Seek(const Slice& user_key) override {
    for (auto& c : children_) c->Seek(user_key);
    valid_ = false;
    FindNextVisibleEntry();
  }

  void Next() override {
    if (!valid_) return;
    FindNextVisibleEntry();
  }

private:
  // 找出下一个应当输出的 entry，并填入 cur_key_ / cur_value_。
  void FindNextVisibleEntry() {
    if (!status_.ok()) return;
    while (true) {
      const int idx = SmallestChild();
      if (idx < 0) {
        // 所有数据源都耗尽 —— 遍历正常结束。
        // 必须在这里清掉 valid_：Next() 进入时 valid_ 是 true，若不置回 false，
        // 调用方的 `for (; Valid(); Next())` 就永远退不出循环，反复读到最后一个
        // key（表现为死循环，而不是干净的遍历结束）。
        valid_ = false;
        return;
      }

      const Slice ikey = children_[idx]->key();
      ParsedInternalKey parsed;
      if (!ParseInternalKey(ikey, &parsed)) {
        status_ = Status::Corruption("db iterator: malformed internal key");
        valid_ = false;
        return;
      }

      if (parsed.sequence > snapshot_) {
        // 该版本比快照新，不可见：推进当前源，继续归并。
        children_[idx]->Next();
        continue;
      }

      // 找到了该 user_key 在快照下最新的可见版本。
      // 用局部变量记录"本轮是否产出"，而不是依赖 valid_ 的残留状态：
      // Next() 进入时 valid_ 是 true（当前位置有效），若这一轮命中墓碑却没把
      // valid_ 清掉，后面的 `if (emitted) return` 就必须靠局部变量判断，
      // 否则会把上一次的 key/value 当成本轮结果返回。
      const bool emitted = (parsed.type == kTypeValue);
      if (emitted) {
        // 对外暴露的是 user_key（用户不该看到 8 字节的版本后缀）。
        cur_key_.assign(parsed.user_key.data(), parsed.user_key.size());
        const Slice v = children_[idx]->value();
        cur_value_.assign(v.data(), v.size());
        // 同时保留完整 internal key：Compaction 要把它原样写进新 SSTable，
        // 而 SSTable 的键必须是 internal key。若这里只留 user_key，compaction
        // 就无法在压缩时保住 version 号，历史快照读会失效。
        cur_internal_key_.clear();
        AppendInternalKey(&cur_internal_key_, parsed);
        valid_ = true;
      }
      // 【必须拷贝 user_key 再传给 SkipRestOfUserKey】
      // parsed.user_key 指向子迭代器内部的 key 缓冲区，而该函数会调用 Next()
      // 改写这个缓冲区。若直接把 Slice 传进去，第二轮循环拿到的 target 就是
      // 悬垂指针，会导致"跳过同 user_key"的判断出错——该 key 可能被输出多次，
      // 或者误吞掉相邻 key。拷贝一份让比较与缓冲区解耦。
      const std::string user_key_copy(parsed.user_key.data(),
                                      parsed.user_key.size());
      // 墓碑（kTypeDeletion）：不输出，也不回退到更老的版本——删除应当对整个
      // user_key 生效。两种情况都要跳过该 key 的全部版本。
      SkipRestOfUserKey(Slice(user_key_copy));
      if (emitted) return;
    }
  }

  // 在所有子迭代器中找出 internal_key 最小者的下标；没有有效源返回 -1。
  //
  // 【为什么用线性扫描而不是堆】
  // 数据源个数 = SSTable 文件数 + 1（W4 无 compaction，文件数不多）。
  // 线性扫描是 O(k) 每输出一个 entry，k 很小时完全够用，且实现简单、不需要
  // 堆的比较器类型体操。等 W5 引入 compaction、文件数上一个量级后再换最小堆，
  // 接口无需改动。
  int SmallestChild() const {
    int best = -1;
    Slice best_key;
    for (size_t i = 0; i < children_.size(); ++i) {
      if (!children_[i]->Valid()) continue;
      if (best < 0 || icmp_->Compare(children_[i]->key(), best_key) < 0) {
        best = static_cast<int>(i);
        best_key = children_[i]->key();
      }
    }
    return best;
  }

  // 跳过所有数据源中 user_key == target 的剩余 entry。
  //
  // 因为同一 user_key 的所有版本在全局 internal_key 序中是连续的、且最新版本
  // 排在最前，所以在输出了其中一个之后把各源里该 key 的部分全部跳过，就能保证
  // 该 user_key 只被输出一次。墓碑场景也靠这一步做到"删除遮挡老版本"。
  void SkipRestOfUserKey(const Slice& target) {
    for (auto& c : children_) {
      while (c->Valid()) {
        ParsedInternalKey p;
        // 解析失败时不静默跳过：留给上层 FindNextVisibleEntry 上报。
        if (!ParseInternalKey(c->key(), &p)) return;
        if (icmp_->user_comparator()->Compare(p.user_key, target) != 0) break;
        c->Next();
      }
    }
  }

  const InternalKeyComparator* icmp_;
  SequenceNumber snapshot_;
  std::vector<std::unique_ptr<Iterator>> children_;
  std::string cur_key_;          // 对外的 user_key
  std::string cur_value_;
  std::string cur_internal_key_;  // Compaction 用：含 seq/type 后缀的完整键
  bool valid_ = false;
  Status status_;
};

// ===========================================================================
// MemTableIteratorAdapter —— 把 MemTable::Iterator 适配成统一的 Iterator
//
// 【为什么要一层适配】
// MemTable::Iterator 的 key() 是 internal_key（这点正合归并需要），但它的
// Seek 接受的是 user_key，且它本身不满足 Iterator 接口（定位语义、Next 的
// 行为约定都不一致）。这层适配把差异抹平：key() 统一返回 internal_key，
// Seek 的 user_key 目标则转换成按 internal_key 定位。
//
// 【为什么适配器自己持有 MemTable 引用】
// 迭代器可能在 flush 换表之后继续使用；若此时没人持有旧 MemTable 的引用，
// 它会被 delete，迭代器就访问了悬垂内存。因此适配器构造时 Ref、析构时 Unref，
// 把"迭代期间 MemTable 必须存活"这个约束落到代码里，而不是靠调用方记得。
// ===========================================================================
class MemTableIteratorAdapter : public Iterator {
public:
  explicit MemTableIteratorAdapter(MemTable* mem) : mem_(mem), iter_(mem) {
    mem_->Ref();
  }
  ~MemTableIteratorAdapter() override {
    if (mem_ != nullptr) mem_->Unref();
  }

  MemTableIteratorAdapter(const MemTableIteratorAdapter&) = delete;
  MemTableIteratorAdapter& operator=(const MemTableIteratorAdapter&) = delete;

  bool Valid() const override { return iter_.Valid(); }

  void SeekToFirst() override { iter_.SeekToFirst(); }

  // MemTable::Iterator::Seek 本就接受 user_key，与接口语义一致，直接转发即可。
  void Seek(const Slice& target) override { iter_.Seek(target); }

  void Next() override { iter_.Next(); }

  Slice key() const override { return iter_.internal_key(); }
  Slice value() const override { return iter_.value(); }

  // MemTable 内部键解析失败属于内存损坏；本适配器无法区分"迭代结束"与"损坏"，
  // 故保持 OK。损坏会在 DBIterator 解析该 internal_key 时被捕获并上报。
  Status status() const override { return Status::OK(); }

private:
  MemTable* const mem_;
  MemTable::Iterator iter_;
};

}  // namespace tinystore