#pragma once

#include <cstddef>

#include "tinystore/env.h"
#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {
namespace log {

// ===========================================================================
// WAL 物理记录格式（log::Writer 写入 / log::Reader 读取）
// ===========================================================================
//
// 【为什么 WAL 要按 block 切块 + 分片】
// WAL 是纯追加的日志。如果一条 record 直接写，恢复时只能"从文件头扫到尾"，
// 但崩溃往往发生在"写到一半"。为了让"半条 record"不影响后续 record 的恢复，
// 我们把文件切成固定 kBlockSize(32KB) 的块，每条 record 最多填满一个块内剩余
// 空间，放不下的就切成分片（FIRST/MIDDLE/LAST），下一分片从新块开头继续。
// 这样即使最后一个块写了半个 record，只需丢弃那半个块，前面的分片依然能拼回
// 完整的 record——崩溃恢复只需处理"最后一个不完整的块"，复杂度是常数级。
//
// Record 物理布局（每个分片）：
//   | crc32c (4B, 小端) | length (2B, 小端) | type (1B) | data (length 字节) |
//   crc 覆盖的是 (type 字节 + data)，不含 length 本身——这样校验和能直接验证
//   数据内容，而 length 用来切分 payload。
enum RecordType : uint8_t {
  kZeroType = 0,  // 仅用于块内填充（pad），读到即跳过
  kFullType = 1,  // 一条完整 record
  kFirstType = 2,  // 跨块 record 的首分片
  kMiddleType = 3,  // 中间分片
  kLastType = 4,  // 末分片
};

constexpr size_t kBlockSize = 32768;
constexpr int kHeaderSize = 4 + 2 + 1;  // crc + length + type

// ===========================================================================
// Writer —— 把任意长度的 record 切分、加校验和、写入 WritableFile
// ===========================================================================
class Writer {
public:
  // dest 的生命周期由调用方负责，Writer 不拥有它。
  // block_offset：在已有文件（如重放后的 MANIFEST / WAL）上续写时，
  // 传入当前文件大小对 kBlockSize 取模，使新记录从正确的块内偏移继续。
  explicit Writer(WritableFile* dest, uint64_t block_offset = 0)
      : dest_(dest), block_offset_(static_cast<int>(block_offset % kBlockSize)) {}

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // 写入一条 record（可能跨多个物理分片）。返回的 Status 是底层 Append 的结果。
  // 注意：本 Writer 只负责切块与校验，不负责 fsync——fsync 由上层（DB）决定时机。
  Status AddRecord(const Slice& slice);

private:
  Status EmitPhysicalRecord(RecordType type, const Slice& slice, size_t n);

  WritableFile* dest_;
  int block_offset_;  // 当前块内已写的字节数
};

}  // namespace log
}  // namespace tinystore
