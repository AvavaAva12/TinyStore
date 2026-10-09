#pragma once

#include <cstddef>

#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {

// ===========================================================================
// Iterator —— 有序数据的只读遍历接口
//
// 【为什么需要它】
// Get 只能做"点查"：给一个 key，告诉你存不存在、值是多少。但真实业务里大量
// 需求是"范围查询"——按字典序扫 [a, z)、找出所有以 "user:" 开头的 key、
// 做一次全库扫描做数据迁移。没有迭代器，这些都得靠上层自己拿 Get 逐个猜 key，
// 既慢又不可行。
//
// 【为什么是"有序"契约而不是任意遍历】
// 迭代器保证按 internal_key 序输出。但 DB 层暴露给用户的 key 是去掉后缀的
// user_key，而且同一个 user_key 的多个历史版本（不同 sequence）**只输出其中
// 一个**：在给定 snapshot 下可见的最新版本。MVCC 过滤发生在 DB 层，
// SSTable 层看到的仍是完整的、内部有序的键序列。分层过滤的理由见 db_iterator。
//
// 【正反两个方向都可用】
// SeekToFirst / Last / Seek / Next / Prev 五件套都是完整契约。
//
// 【反向遍历为什么需要专门设计】
// 正向遍历可以顺着 next 指针一路向前；反向则要"往回找前驱"，而块格式的前缀
// 压缩让每条 entry 只存"与前一条共享的字节数"，还原前一条的完整 key 需要知道
// 它**之前**那条的完整 key —— 依赖链只能正向解开。因此反向定位采用
// "从最近的重启点重新正向扫一小段、取其中最后一条"的策略（见 block.h 的
// FindLastBefore）。重启点间隔（默认 16）把这个代价限制在常数级，
// 若没有重启点，反向一步就要从块首重扫，退化成 O(n) per step。
//
// 跳表与 MemTable 的反向遍历则是天然的：SkipList 的查找路径能直接定位
// "严格小于 target 的最大节点"（FindLessThan），无需回扫。
//
// 【key() / value() 的生命周期】
// 返回的 Slice 指向迭代器内部缓冲区，不拥有数据。**在调用 Next/Prev/Seek 之前**
// 它保持有效；迭代器一旦移动位置，之前拿到的 Slice 立即失效。
// 需要跨位置保存时请自行拷贝到 std::string。
// ===========================================================================
class Iterator {
public:
  virtual ~Iterator() = default;

  // 当前位置是否有效。false 表示遍历结束（或尚未开始）。
  virtual bool Valid() const = 0;

  // 定位到第一个 key（要求当前 iterator 持有底层数据）。
  virtual void SeekToFirst() = 0;

  // 定位到最后一个 key。
  virtual void SeekToLast() = 0;

  // 定位到第一个 key >= target。
  //
  // 【target 是 user_key，不是 internal_key】
  // 与 Get(key) 保持同一套对外语义，调用者不必知道 internal_key 的存在。
  // "快照点在哪"由实现自己处理：具体由哪个版本算作"第一个"，取决于该迭代器
  // 被赋予的 snapshot（例如 DBIterator 会跳过比快照新的版本）。
  virtual void Seek(const Slice& target) = 0;

  // 前进到下一个 key。调用前要求 Valid() == true。
  virtual void Next() = 0;

  // 后退到上一个 key。调用前要求 Valid() == true。
  //
  // 【遍历结束的约定与 Next 一致】
  // 已经位于第一个 key 时调用 Prev()，Valid() 变为 false。反向遍历到头
  // 与正向遍历到尾是同一种"正常结束"，调用方不该把它当错误。
  virtual void Prev() = 0;

  // 当前 key / value。要求 Valid() == true。
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;

  // 遍历过程中的状态。Valid() 为 false 时用它区分"正常遍历结束"与
  // "出错中止"——例如读到损坏的块，前者应返回 OK，后者返回 Corruption。
  virtual Status status() const = 0;

  // 便捷判断：当前位置是否以指定前缀开头。要求 Valid() == true。
  bool starts_with(const Slice& prefix) const {
    return Valid() && key().starts_with(prefix);
  }
};

}  // namespace tinystore