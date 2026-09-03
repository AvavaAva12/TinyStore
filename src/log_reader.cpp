#include "tinystore/log_reader.h"

#include "tinystore/coding.h"
#include "tinystore/crc32c.h"

namespace tinystore {
namespace log {

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      initial_offset_(initial_offset),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      buffer_() {}

bool Reader::SkipToInitialBlock() {
  // WAL 总是从头读（initial_offset_ == 0）。其它偏移（MANIFEST）后续按需扩展。
  return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  if (last_record_offset_ < initial_offset_) {
    if (!SkipToInitialBlock()) return false;
  }

  scratch->clear();
  record->clear();
  bool in_fragment = false;

  while (true) {
    const uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size();

    Slice fragment;
    RecordType type = kZeroType;
    Status s = ReadPhysicalRecord(&fragment, &type);
    if (!s.ok()) {
      // EOF 或不可恢复错误：若正处于分片中，上报截断
      if (in_fragment && reporter_) {
        reporter_->Corruption(scratch->size(),
                              Status::Corruption("truncated record"));
      }
      buffer_ = Slice();
      return false;
    }

    switch (type) {
      case kFullType: {
        if (in_fragment && reporter_) {
          reporter_->Corruption(scratch->size(),
                                Status::Corruption("partial record missing end"));
        }
        in_fragment = false;
        scratch->assign(fragment.data(), fragment.size());
        *record = Slice(scratch->data(), scratch->size());
        last_record_offset_ = physical_record_offset;
        return true;
      }
      case kFirstType: {
        if (in_fragment && reporter_) {
          reporter_->Corruption(scratch->size(),
                                Status::Corruption("partial record missing end"));
        }
        in_fragment = true;
        scratch->assign(fragment.data(), fragment.size());
        break;
      }
      case kMiddleType: {
        if (!in_fragment && reporter_) {
          reporter_->Corruption(fragment.size(),
                                Status::Corruption("missing start of fragment"));
        } else if (in_fragment) {
          scratch->append(fragment.data(), fragment.size());
        }
        break;
      }
      case kLastType: {
        if (!in_fragment && reporter_) {
          reporter_->Corruption(fragment.size(),
                                Status::Corruption("missing start of fragment"));
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(scratch->data(), scratch->size());
          last_record_offset_ = physical_record_offset;
          return true;
        }
        break;
      }
      case kZeroType:
        // 块内填充，跳过
        break;
      default: {
        // 未知类型：损坏
        if (reporter_) {
          reporter_->Corruption(fragment.size(),
                                Status::Corruption("unknown record type"));
        }
        buffer_ = Slice();
        return false;
      }
    }
  }
}

Status Reader::ReadPhysicalRecord(Slice* result, RecordType* record_type) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // 重新从块边界读一块（丢弃不足一个头的残片，残片只可能是块末填充）
        buffer_ = Slice();
        const size_t to_read = kBlockSize - (end_of_buffer_offset_ % kBlockSize);
        Status s = file_->Read(to_read, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!s.ok()) return s;
        if (buffer_.size() < kHeaderSize) {
          eof_ = true;
          // 文件尾的不足一个头的残片按 EOF 处理，不报损坏
          return Status::Corruption("EOF");
        }
      } else {
        return Status::Corruption("EOF");
      }
    }

    const char* const header = buffer_.data();
    const uint32_t a = static_cast<uint8_t>(header[4]);
    const uint32_t b = static_cast<uint8_t>(header[5]);
    const uint32_t length = a | (b << 8);
    const uint32_t expected_crc = DecodeFixed32(header);
    *record_type = static_cast<RecordType>(header[6]);

    // 注意顺序：必须先做长度检查，再 remove_prefix。此时 buffer_ 仍包含 7 字节
    // 头部，因此用 kHeaderSize + length 与 buffer_.size() 比较。若把 remove_prefix
    // 提前到检查之前，buffer_ 会少算 7 字节，导致跨越块边界的大分片被误判为长度越界。
    if (kHeaderSize + length > buffer_.size()) {
      // 分片的数据超出了当前已读缓冲（不完整 / 损坏）
      const size_t drop = buffer_.size();
      buffer_ = Slice();
      if (reporter_) {
        reporter_->Corruption(drop, Status::Corruption("bad record length"));
      }
      return Status::Corruption("bad record length");
    }

    *result = Slice(buffer_.data() + kHeaderSize, length);
    buffer_.remove_prefix(kHeaderSize + length);

    if (checksum_ && *record_type != kZeroType) {
      // crc 覆盖 (type 字节 + data)
      const uint32_t actual =
          crc32c::Extend(crc32c::Extend(0, header + 6, 1), result->data(), length);
      if (actual != expected_crc) {
        const size_t drop = buffer_.size();
        buffer_ = Slice();
        if (reporter_) {
          reporter_->Corruption(drop + kHeaderSize + length,
                                Status::Corruption("checksum mismatch"));
        }
        return Status::Corruption("checksum mismatch");
      }
    }
    return Status::OK();
  }
}

}  // namespace log
}  // namespace tinystore
