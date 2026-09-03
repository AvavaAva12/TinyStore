#include "tinystore/log_writer.h"

#include <cassert>

#include "tinystore/coding.h"
#include "tinystore/crc32c.h"

namespace tinystore {
namespace log {

Status Writer::EmitPhysicalRecord(RecordType type, const Slice& slice,
                                 size_t n) {
  assert(n <= 0xffff);  // length 字段只有 2 字节
  assert(static_cast<size_t>(block_offset_) + kHeaderSize <= kBlockSize);

  char buf[kHeaderSize];
  // crc 覆盖 (type 字节 + data)，不含 length 本身
  uint32_t crc = crc32c::Extend(0, reinterpret_cast<const char*>(&type), 1);
  crc = crc32c::Extend(crc, slice.data(), n);
  EncodeFixed32(buf, crc);
  buf[4] = static_cast<char>(n & 0xff);
  buf[5] = static_cast<char>((n >> 8) & 0xff);
  buf[6] = static_cast<char>(type);

  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) s = dest_->Append(Slice(slice.data(), n));
  block_offset_ += static_cast<int>(kHeaderSize + n);
  return s;
}

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();
  bool begin = true;

  do {
    int leftover = static_cast<int>(kBlockSize) - block_offset_;
    if (leftover < kHeaderSize) {
      if (leftover > 0) {
        // 用全零（kZeroType）填充满当前块到边界；读到时会被跳过
        static char zeroes[kHeaderSize] = {0};
        Status s = dest_->Append(Slice(zeroes, leftover));
        if (!s.ok()) return s;
      }
      block_offset_ = 0;
    }

    const int avail =
        static_cast<int>(kBlockSize) - block_offset_ - kHeaderSize;
    const int frag_length =
        (left <= static_cast<size_t>(avail)) ? static_cast<int>(left) : avail;
    const bool end = (frag_length == static_cast<int>(left));

    RecordType type;
    if (begin && end)
      type = kFullType;
    else if (begin)
      type = kFirstType;
    else if (end)
      type = kLastType;
    else
      type = kMiddleType;

    Status s = EmitPhysicalRecord(type, Slice(ptr, frag_length), frag_length);
    if (!s.ok()) return s;

    ptr += frag_length;
    left -= frag_length;
    begin = false;
  } while (left > 0);

  return Status::OK();
}

}  // namespace log
}  // namespace tinystore
