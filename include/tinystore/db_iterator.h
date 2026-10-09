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
//
// 【Compaction 模式：为什么需要另一套规则】
// 上面的规则对"用户遍历"是对的，但对 compaction 是错的——compaction 的目的是
// **丢掉不再需要的版本以减少空间**，若照搬"只留快照点最新版"的规则，
// 就会把比快照更新的版本一起丢掉，直接丢数据。因此 compaction 模式下：
//
//   * seq > snapshot 的版本：**全部保留**（它们是未来读者需要的）
//   * seq <= snapshot 的版本：只保留该 user_key 的**第一个**（快照点可见的最新版）
//   * 更老的版本：一律丢弃（对任何活跃快照都不可见）
//
// snapshot 传的是"最早活跃快照"，于是这个规则恰好保住了每个活跃快照的视图。
//
// 【非最底层的 compaction 必须保留墓碑】
// drop_tombstones=false 时墓碑照常写出。原因：L1->L2 归并后，墓碑下沉到 L2，
// 而该 key 可能还有更老的版本躺在 L3。若此刻丢掉墓碑，L3 的旧值就会"复活"。
// 只有沉到最底层的压缩才允许丢弃墓碑——那里已经没有任何更老的层了。
// ===========================================================================
class DBIterator : public Iterator {
public:
  DBIterator(const InternalKeyComparator* icmp, SequenceNumber snapshot,
             bool for_compaction = false, bool drop_tombstones = true)
      : icmp_(icmp),
        snapshot_(snapshot),
        for_compaction_(for_compaction),
        drop_tombstones_(drop_tombstones) {}

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
    ResetScanState();
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
    ResetScanState();
    FindNextVisibleEntry();
  }

  // 重新定位后，compaction 模式的"上一个 user_key"状态不再适用：
  // 子迭代器已被各自跳到目标位置，归并序的连续性假设要重新开始建立。
  void ResetScanState() {
    valid_ = false;
    have_last_user_key_ = false;
    user_key_done_ = false;
  }

  void Next() override {
    if (!valid_) return;
    FindNextVisibleEntry();
  }

  // ---- 反向遍历 ----

  // 【为什么反向不能照搬正向的"逐条过滤"】
  // 归并序是 internal_key 升序：同 user_key 的版本按 seq **降序**排列。
  // 反向走时，同一 user_key 的版本按 seq **升序**出现（先老后新）。
  //
  // 正向的做法（跳过 seq > snapshot 的，再跳过整个 user_key 的剩余）是对的；
  // 反向照搬就错了：先遇到的恰恰是**最老**的版本，直接输出会拿到旧值。
  //
  // 因此反向改为"整个 user_key 一起处理"：
  //   1. 反向扫过该 user_key 的全部版本（各源都退到它之前）；
  //   2. 扫的过程中持续记录 seq <= snapshot 的版本 —— 反向是 seq 升序，
  //      最后记录下的那个正是 seq 最大的可见版本；
  //   3. 全部源都退到下一个 user_key 后，输出记录的版本。
  //
  // 代价是一次 user_key 要扫过它的所有版本（总代价仍是 O(总 entry 数)，
  // 与正向同阶）；换来的是语义与正向完全一致，且不需要为每个 user_key 缓存
  // 全部版本（那会退化成"把整个数据库读进内存"）。
  void SeekToLast() override {
    if (for_compaction_) {
      status_ = Status::InvalidArgument(
          "db iterator: reverse iteration is compaction-mode only forward");
      valid_ = false;
      return;
    }
    for (auto& c : children_) c->SeekToLast();
    ResetScanState();
    FindPrevVisibleEntry();
  }

  void Prev() override {
    if (!Valid()) return;
    FindPrevVisibleEntry();
  }

private:
  // 把当前 entry 填入对外可见的 cur_key_ / cur_value_ / cur_internal_key_。
  void Emit(const ParsedInternalKey& parsed, const Slice& value) {
    // 对外暴露的是 user_key（用户不该看到 8 字节的版本后缀）。
    cur_key_.assign(parsed.user_key.data(), parsed.user_key.size());
    cur_value_.assign(value.data(), value.size());
    // 同时保留完整 internal key：Compaction 要把它原样写进新 SSTable，
    // 而 SSTable 的键必须是 internal key。若这里只留 user_key，compaction
    // 就无法在压缩时保住 version 号，历史快照读会失效。
    cur_internal_key_.clear();
    AppendInternalKey(&cur_internal_key_, parsed);
    valid_ = true;
  }

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

      // 【必须拷贝 user_key 再往下传】
      // parsed.user_key 指向子迭代器内部的 key 缓冲区，而后续逻辑会调用 Next()
      // 改写这个缓冲区。若直接把 Slice 传出去，第二轮拿到的就是悬垂指针，
      // "跳过同 user_key"的判断会出错——该 key 可能被输出多次，或误吞相邻 key。
      const std::string user_key_copy(parsed.user_key.data(),
                                      parsed.user_key.size());

      if (!for_compaction_) {
        FindNextUserEntry(idx, parsed, user_key_copy);
        if (!status_.ok()) return;
        if (valid_) return;  // 本轮产出了结果，让调用方消化
        continue;            // 本轮没产出（跳过了墓碑），继续归并
      }

      FindNextCompactionEntry(idx, parsed, user_key_copy);
      if (!status_.ok()) return;
      if (valid_) return;
    }
  }

  // 用户遍历模式：只输出该 user_key 在快照下最新的可见版本，遇到墓碑整键跳过。
  //
  // 返回后 valid_ 为 true 表示本轮有产出；false 表示"这条不该输出，继续找"。
  void FindNextUserEntry(int idx, const ParsedInternalKey& parsed,
                         const std::string& user_key) {
    if (parsed.sequence > snapshot_) {
      // 该版本比快照新，不可见：推进当前源，交给上层继续归并。
      // 必须清 valid_：上层靠它判断"本轮有没有产出"。不清的话 Next() 进入时
      // 残留的 true 会让归并循环误以为已经产出结果而直接返回，
      // 表现为遍历提前停在"最后一个 key"上多吐一次。
      valid_ = false;
      children_[idx]->Next();
      return;
    }
    // 找到了该 user_key 在快照下最新的可见版本。
    // 用局部变量记录"本轮是否产出"，而不是依赖 valid_ 的残留状态：
    // Next() 进入时 valid_ 是 true（当前位置有效），若这一轮命中墓碑却没把
    // valid_ 清掉，后面的 `if (emitted) return` 就必须靠局部变量判断，
    // 否则会把上一次的 key/value 当成本轮结果返回。
    const bool emitted = (parsed.type == kTypeValue);
    if (emitted) Emit(parsed, children_[idx]->value());
    // 墓碑（kTypeDeletion）：不输出，也不回退到更老的版本——删除应当对整个
    // user_key 生效。两种情况都要跳过该 key 的全部版本。
    SkipRestOfUserKey(Slice(user_key));
    if (!emitted) valid_ = false;
  }

  // Compaction 模式：保留"全部更新版本 + 快照点最新版"，丢弃其余。
  //
  // 【状态机】同一 user_key 的版本在归并序里连续出现（各源都用同一个
  // InternalKeyComparator，故全局有序、同键连续），因此只需记住"上一个处理的
  // user_key"和"它的保留版本是否已发完"，不必为每个 user_key 收集全部版本。
  void FindNextCompactionEntry(int idx, const ParsedInternalKey& parsed,
                               const std::string& user_key) {
    // 换了新的 user_key：重置该键的"已发完"标记。
    if (!have_last_user_key_ || last_user_key_ != user_key) {
      last_user_key_ = user_key;
      have_last_user_key_ = true;
      user_key_done_ = false;
    }

    if (user_key_done_) {
      // 该 user_key 要保留的版本都已输出，这条是更老的、任何活跃快照都看不到
      // 的版本 —— 丢弃。同上，必须清 valid_ 让上层继续归并。
      valid_ = false;
      children_[idx]->Next();
      return;
    }

    // seq <= snapshot 的第一个版本就是"快照点可见的最新版"，保留它；
    // 它之后该 user_key 的所有版本都可以安全丢弃。
    const bool finished_this_user_key = (parsed.sequence <= snapshot_);
    if (finished_this_user_key) user_key_done_ = true;

    const bool is_tombstone = (parsed.type == kTypeDeletion);
    const bool emitted = !is_tombstone || !drop_tombstones_;
    if (emitted) Emit(parsed, children_[idx]->value());

    if (finished_this_user_key) {
      // 快照点版本已产出：其余源里该 user_key 的剩余部分（含同一版本的重复
      // 出现）一次性跳过，保证同一 user_key 只写一次。
      SkipRestOfUserKey(Slice(user_key));
    } else {
      // 还有比快照更新的版本要保留，只推进当前源（其余源的同键版本可能序号
      // 更大，留给后续轮次；序号更小的则已在前面的轮次处理过）。
      children_[idx]->Next();
    }
    if (!emitted) valid_ = false;
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

  // 反向归并：internal_key 最大者。没有有效源返回 -1。
  int LargestChild() const {
    int best = -1;
    Slice best_key;
    for (size_t i = 0; i < children_.size(); ++i) {
      if (!children_[i]->Valid()) continue;
      if (best < 0 || icmp_->Compare(children_[i]->key(), best_key) > 0) {
        best = static_cast<int>(i);
        best_key = children_[i]->key();
      }
    }
    return best;
  }

  // 反向定位下一个应输出的 user_key（见 SeekToLast 的注释）。
  void FindPrevVisibleEntry() {
    if (!status_.ok()) return;
    while (true) {
      const int idx = LargestChild();
      if (idx < 0) {  // 所有源都退到开头 —— 反向遍历正常结束
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
      // 必须拷贝：下面各源 Prev() 会改写子迭代器的 key 缓冲区。
      const std::string user_key(parsed.user_key.data(), parsed.user_key.size());

      // 反向扫过该 user_key 的全部版本，边扫边记下快照下可见的最新版本。
      //
      // 【为什么取"最后一个"而不是"第一个"】
      // 归并序里同 user_key 的版本按 seq 降序排列，反向走就是 seq 升序 ——
      // 也就是**先遇到最老的版本**。所以不能一见满足条件就锁定，否则拿到的是
      // 历史旧值。必须持续更新，最后留下的才是 seq 最大的可见版本。
      //
      // 【为什么必须深拷贝 user_key】
      // best_user_key 会跨 c->Prev() 存活，而 ParsedInternalKey 里的 Slice 只是
      // 指向子迭代器的 key 缓冲区，Prev() 一执行那块内存就被下一个 entry 覆盖。
      // 直接把 Slice 存进 best_parsed 会让它在循环结束后变成悬垂指针。
      bool found = false;
      std::string best_user_key;
      std::string best_value;
      SequenceNumber best_seq = 0;
      ValueType best_type = kTypeValue;
      for (auto& c : children_) {
        while (c->Valid()) {
          ParsedInternalKey p;
          if (!ParseInternalKey(c->key(), &p)) {
            status_ = Status::Corruption("db iterator: malformed internal key");
            valid_ = false;
            return;
          }
          if (icmp_->user_comparator()->Compare(p.user_key, Slice(user_key)) != 0) {
            break;
          }
          if (p.sequence <= snapshot_) {
            found = true;
            best_user_key.assign(p.user_key.data(), p.user_key.size());
            best_value.assign(c->value().data(), c->value().size());
            best_seq = p.sequence;
            best_type = p.type;
          }
          c->Prev();
        }
      }

      // 最新可见版本是墓碑 -> 整个 user_key 不输出（删除对整个键生效），
      // 继续找更小的 user_key。没有可见版本（全部比快照新）同样跳过。
      if (found && best_type == kTypeValue) {
        ParsedInternalKey best;
        best.user_key = Slice(best_user_key);
        best.sequence = best_seq;
        best.type = best_type;
        Emit(best, Slice(best_value));
        return;
      }
    }
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

  // --- Compaction 模式的状态（见 FindNextCompactionEntry）---
  bool for_compaction_ = false;
  bool drop_tombstones_ = true;
  std::string last_user_key_;
  bool have_last_user_key_ = false;
  bool user_key_done_ = false;
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
// 把 MemTable 包成一个 Iterator 源，供 DBIterator 归并。
//
// 【引用语义：接管而非新增】
// 调用方必须**已经持有一个引用**，并在构造期间保证 mem 存活（通常是在
// mem_mutex_ 临界区内完成 "load + Ref"）。本类在析构时归还这一个引用 ——
// 它是整个迭代器存活期间该 MemTable 不被 flush 释放的唯一保证。
//
// 【为什么不在构造函数里 Ref】
// DBImpl::NewIterator 必须在 mem_mutex_ 临界区内完成 "load + Ref"，才能避免
// "load 到 m 之后、Ref 之前"被 flush 换表并 delete（use-after-free）。若本类
// 构造时再 Ref 一次，就变成两次 Ref、只有一次 Unref，永久泄漏一个引用 ——
// 这个错误很隐蔽，因为功能测试全绿，只有 LeakSanitizer 能发现。
class MemTableIteratorAdapter : public Iterator {
public:
  explicit MemTableIteratorAdapter(MemTable* mem) : mem_(mem), iter_(mem) {}
  ~MemTableIteratorAdapter() override {
    if (mem_ != nullptr) mem_->Unref();
  }

  MemTableIteratorAdapter(const MemTableIteratorAdapter&) = delete;
  MemTableIteratorAdapter& operator=(const MemTableIteratorAdapter&) = delete;

  bool Valid() const override { return iter_.Valid(); }

  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }

  // MemTable::Iterator::Seek 本就接受 user_key，与接口语义一致，直接转发即可。
  void Seek(const Slice& target) override { iter_.Seek(target); }

  void Next() override { iter_.Next(); }

  // 反向直接透传：MemTable::Iterator 的 Prev 走 SkipList 的 FindLessThan，
  // 是沿查找路径定位"严格小于当前 key 的最大节点"，无需回扫前缀链。
  void Prev() override { iter_.Prev(); }

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