#pragma once

#include <cstdint>
#include <string>

#include "tinystore/internal_key.h"
#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {

class MemTable;  // WriteBatchInternal::InsertInto 用到，前向声明避免耦合

// ===========================================================================
// WriteBatch —— 把一组 Put/Delete 打包成一个可原子回放/落盘的字节串
// ===========================================================================
//
// 【为什么需要它】
// 单个写事务里可能含多个 key 的修改（如"转账"要改两个账户）。如果逐个写，
// 中途崩溃会导致"只改了一半"的部分写，破坏原子性。WriteBatch 把这些修改
// 编码成一条字节串，配合 WAL 一次落盘、一次回放，对外呈现"要么全做、要么全不做"。
//
// 【字节格式】
//   | uint64 sequence (8B) | uint32 count (4B) | records... |
//   sequence：该 batch 起始序号；count：记录条数；二者构成 12 字节头部。
//   每条 record：
//     | tag (1B: kTypeValue / kTypeDeletion) |
//       [varint(key_len)][key] [varint(val_len)][value] |
//   删除记录的 val_len = 0（仅保留零长度前缀，无 value 字节）。
//
// 【设计取舍：Handler 回调而非直接依赖 MemTable】
// Iterate() 不直接认识 MemTable/WAL，它只把一条条 Put/Delete 喂给一个
// WriteBatch::Handler。这样同一份编码既能被回放到 MemTable（写入内存），
// 也能被 WAL 读取后回放（恢复），甚至将来被复制给从节点（分布式）。
// 这是"数据格式"与"施加动作"解耦的典型做法。
class WriteBatch {
public:
  // 回放处理器：Iterate 每解出一条记录就回调一次
  class Handler {
  public:
    virtual ~Handler() = default;
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch() { Clear(); }

  // 追加一条 Put / Delete（同时维护头部 count）
  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);

  // 清空为初始状态（header 全 0，count = 0）
  void Clear();

  // 记录条数（解码头部 count 字段）
  uint32_t Count() const;

  // 完整字节串（含头部），用于序列化进 WAL
  const std::string& Contents() const { return rep_; }

  // 用一段已编码的字节串直接覆盖内容（用于 WAL 回放：读出的 record
  // 本身就是一条 WriteBatch 字节串，原样灌进来再 Iterate/InsertInto 即可）
  void SetContents(const Slice& contents);

  // 解码 record 段，逐条回调 handler。遇到非法格式返回 Corruption。
  Status Iterate(Handler* handler) const;

  // WriteBatchInternal 需要直接读写头部与 rep_（分配 sequence、合并 batch）
  friend struct WriteBatchInternal;

private:
  std::string rep_;  // [seq(8)][count(4)][records...]
};

// ===========================================================================
// WriteBatchInternal —— 操作 WriteBatch 头部的辅助函数（DB 层使用）
// ===========================================================================
struct WriteBatchInternal {
  // 起始 sequence（落盘前由 DB 分配并写入）
  static SequenceNumber Sequence(const WriteBatch* batch);
  static void SetSequence(WriteBatch* batch, SequenceNumber seq);

  // 把 batch 里的所有记录回放到 memtable：
  // 从 batch 的起始 sequence 开始，逐条自增分配 sequence 后写入 MemTable。
  // 回放完把 batch 自身的 sequence 推进到最后一条之后，方便下一个 batch 续接。
  // 把 batch 的每条 record 逐条灌入 memtable。
  //
  // 【返回 Status 是必需的，不是可选的严谨】
  // batch 内容损坏时 Iterate 会返回 Corruption 且**一条都没插入**。调用方若忽略
  // 本返回值，仍会按头部 count 推进 sequence，于是 last_sequence_ 被推到从未写入
  // 的区间（sequence 空洞），同时这批数据静默丢失。两个调用点都必须检查：
  //   * WAL 重放（db.cpp）—— 遇到损坏记录应停止重放且不推进快照点
  //   * 写路径（db.cpp Write）—— 自己刚构造的 batch 不可能损坏，OK 即忽略
  static Status InsertInto(const WriteBatch* batch, MemTable* memtable);

  // 把 src 的所有 record 追加到 dst（用于 W3 的 Group Commit 合并多个写）
  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace tinystore
