#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "tinystore/env.h"
#include "tinystore/log_writer.h"  // 引入 RecordType / kBlockSize / kHeaderSize
#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {
namespace log {

// 损坏报告回调：恢复过程中遇到坏 record / 截断的 record 时通知调用方。
// 实现可据此计数、打日志或中止恢复。允许传 nullptr（不报告）。
class Reporter {
public:
  virtual ~Reporter() = default;
  // bytes：本次丢弃的字节数；status：原因
  virtual void Corruption(size_t bytes, const Status& status) = 0;
};

// ===========================================================================
// Reader —— 从 SequentialFile 读出完整的逻辑 record（重组分片 + 校验 crc）
// ===========================================================================
//
// 【与 Writer 配套】
// 读取时把 FIRST/MIDDLE/LAST 分片拼回一条完整 record，校验每个分片的 crc，
// 遇到坏分片调用 Reporter。
//
// 【三种结尾必须区分开（本类的核心契约）】
// ReadRecord 返回 false 时，调用方**必须**用 status() 追问原因，因为三种情况
// 的处理方式完全相反：
//
//   1. status().ok()            —— 正常读到文件尾。正常结束。
//   2. status() 是 Corruption   —— **尾部残缺**：最后一条记录的长度超出文件末尾。
//                                 这是"写到一半就崩溃"的正常后果（fsync 只保证
//                                 已返回 OK 的写入，那条半条记录本来就没提交），
//                                 **不当作损坏**，丢弃即可。
//   3. status() 是 IOError       —— 读文件失败，必须上报。
//
//   还有一类必须报错的：文件**中段**损坏（CRC 不匹配、长度越界、未知类型）。
//   它会让调用方的重放提前终止且被误判为"正常读完"，从而把状态回滚——
//   这是丢数据的经典形态。MANIFEST 上尤其致命：log_number 会退回到更早的值，
//   恢复逻辑随即把本该有效的新 WAL 与已登记的 SSTable **双向删除**。
//
// 【为什么 Reporter 不够】
// Reporter 只是可选的日志回调（允许传 nullptr），它不影响 ReadRecord 的返回值。
// 把它当作错误通道，等于让"是否报错"取决于调用方有没有传日志——不传就静默。
// 因此错误状态必须走 status()，与 Reporter 解耦。
class Reader {
public:
  // file / reporter 的生命周期由调用方负责。checksum=true 时校验 crc。
  // initial_offset：从文件某个偏移之后开始读（用于 MANIFEST / 多段日志，WAL 用 0）。
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset = 0);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // 读出下一条完整 record。返回 false 时必须查 status() 区分原因（见类说明）。
  bool ReadRecord(Slice* record, std::string* scratch);

  // 上一次 ReadRecord 返回 false 的原因；返回 true 或正常读到文件尾时为 OK。
  const Status& status() const { return status_; }

private:
  // ReadPhysicalRecord 的三态结果。
  //
  // 之所以不用 Status 表达 EOF：Status 没有"正常结束"这个类别，早期实现曾用
  // Status::Corruption("EOF") 表示读到文件尾，结果 EOF 与真损坏在
  // ReadRecord 层面完全无法区分——这正是本类要解决的核心问题。
  enum class PhysicalResult {
    kOk,      // 成功读出一个物理分片
    kEof,     // 正常读到文件尾，或最后一条记录尾部残缺（均可安全丢弃）
    kCorrupt, // 真损坏 / IO 失败，必须上报
  };

  // 从文件读出一个物理分片（一个 block 内的 [header|data]）。
  // 返回 kOk 时 *fragment 指向分片数据（位于 backing_store_），*record_type 为类型。
  PhysicalResult ReadPhysicalRecord(Slice* fragment, RecordType* record_type);

  // 跳到 initial_offset 所在块的起点（initial_offset=0 时直接成功）
  bool SkipToInitialBlock();

  SequentialFile* file_;
  Reporter* reporter_;
  bool const checksum_;
  char backing_store_[kBlockSize];  // 读缓冲（块对齐）

  uint64_t const initial_offset_;
  bool eof_;                          // 已读到文件尾
  uint64_t last_record_offset_;      // 上一条完整 record 的起始偏移
  uint64_t end_of_buffer_offset_;    // backing_store_ 中已读数据对应的文件尾偏移
  Slice buffer_;                     // 当前块内尚未消费的字节（指向 backing_store_）
  Status status_;                    // 最近一次失败的���因；OK 表示正常
};

}  // namespace log
}  // namespace tinystore
