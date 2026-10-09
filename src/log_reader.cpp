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
      buffer_(),
      status_() {}

bool Reader::SkipToInitialBlock() {
  // WAL 总是从头读（initial_offset_ == 0）。其它偏移（MANIFEST）后续按需扩展。
  return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  status_ = Status::OK();
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
    const PhysicalResult r = ReadPhysicalRecord(&fragment, &type);
    if (r != PhysicalResult::kOk) {
      // 只在**真损坏**时上报分片残缺。尾部残缺（kEof）是崩溃现场的正常形态，
      // 每当恢复都会发生，把它报成损坏会让真正的损坏淹没在噪声里。
      if (r == PhysicalResult::kCorrupt && in_fragment && reporter_) {
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
        // 未知类型：真损坏（尾部残缺已在上面的长度检查里被归为 kEof）
        status_ = Status::Corruption("unknown record type");
        if (reporter_) {
          reporter_->Corruption(fragment.size(), status_);
        }
        buffer_ = Slice();
        return false;
      }
    }
  }
}

Reader::PhysicalResult Reader::ReadPhysicalRecord(Slice* result,
                                                RecordType* record_type) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // 重新从块边界读一块（丢弃不足一个头的残片，残片只可能是块末填充）
        buffer_ = Slice();
        const size_t to_read = kBlockSize - (end_of_buffer_offset_ % kBlockSize);
        Status s = file_->Read(to_read, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!s.ok()) {
          status_ = s;  // IO 失败：必须让调用方知道，不能冒充 EOF
          return PhysicalResult::kCorrupt;
        }
        if (buffer_.size() < kHeaderSize) {
          eof_ = true;
          return PhysicalResult::kEof;
        }
      } else {
        return PhysicalResult::kEof;
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
      const size_t drop = buffer_.size();
      buffer_ = Slice();
      // 【尾部残缺 vs 中段损坏的分界】
      // eof_ 为真意味着文件的所有字节都已在缓冲里，此时"声明长度超出剩余字节"
      // 只能是**最后一条记录写到一半就崩溃**了。这属于 fsync 的正常边界：
      // 那条记录从未返回 OK，恢复时本就不该生效，丢弃是正确的。
      //
      // 反之（eof_ 为假）说明后面还有数据，长度却越界 —— 这是真损坏，必须上报。
      // 若把它也当 EOF，重放会提前终止并被判为"正常读完"，MANIFEST 的
      // log_number 会退回到更早的值，恢复逻辑随即把本该有效的新 WAL 与
      // 已登记的 SSTable 双向删除 —— 丢数据且全程无任何错误返回。
      if (eof_) return PhysicalResult::kEof;
      status_ = Status::Corruption("bad record length");
      if (reporter_) reporter_->Corruption(drop, status_);
      return PhysicalResult::kCorrupt;
    }

    *result = Slice(buffer_.data() + kHeaderSize, length);
    buffer_.remove_prefix(kHeaderSize + length);

    if (checksum_ && *record_type != kZeroType) {
      // crc 覆盖 (type 字节 + data)
      const uint32_t actual =
          crc32c::Extend(crc32c::Extend(0, header + 6, 1), result->data(), length);
      if (actual != expected_crc) {
        // CRC 不匹配**永远**是损坏，不按尾部残缺处理：单纯的截断会被上面的
        // 长度检查先拦下，能走到这里说明内容确实被破坏（或被伪造）。
        const size_t drop = buffer_.size();
        buffer_ = Slice();
        status_ = Status::Corruption("checksum mismatch");
        if (reporter_) {
          reporter_->Corruption(drop + kHeaderSize + length, status_);
        }
        return PhysicalResult::kCorrupt;
      }
    }
    return PhysicalResult::kOk;
  }
}

}  // namespace log
}  // namespace tinystore
