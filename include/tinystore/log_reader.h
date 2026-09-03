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
// 遇到坏分片调用 Reporter。读到一个不完整的"最后一个块"时（崩溃现场）当作
// EOF 正常结束，而不是报错——因为那半个块本就不该被恢复。
//
// 【语义约定】
//   * ReadRecord 返回 true：*record 指向一条完整 record（其内容位于 *scratch，
//     调用方应在下次 ReadRecord 前用完）。
//   * ReadRecord 返回 false：已到文件尾或发生不可恢复错误，停止。
//   * 损坏（crc 不匹配 / 长度越界）通过 Reporter 报告，Reader 自身返回 false。
class Reader {
public:
  // file / reporter 的生命周期由调用方负责。checksum=true 时校验 crc。
  // initial_offset：从文件某个偏移之后开始读（用于 MANIFEST / 多段日志，WAL 用 0）。
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset = 0);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // 读出下一条完整 record。详见类说明。
  bool ReadRecord(Slice* record, std::string* scratch);

private:
  // 从文件读出一个物理分片（一个 block 内的 [header|data]）。
  // 返回 OK 时 *fragment 指向分片数据（位于 backing_store_），*record_type 为类型。
  Status ReadPhysicalRecord(Slice* fragment, RecordType* record_type);

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
};

}  // namespace log
}  // namespace tinystore
