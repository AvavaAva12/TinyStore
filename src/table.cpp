#include "tinystore/table.h"

#include <cstring>
#include <vector>

#include "tinystore/coding.h"
#include "tinystore/crc32c.h"
#include "tinystore/env.h"
#include "tinystore/internal_key.h"

namespace tinystore {
namespace {

constexpr size_t kBlockTrailerSize = 5;  // crc(4) + type(1)
constexpr uint8_t kBlockFullType = 1;

}  // namespace

// ===========================================================================
// BlockHandle / Footer
// ===========================================================================

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  uint64_t off = 0, sz = 0;
  if (!GetVarint64(input, &off) || !GetVarint64(input, &sz)) {
    return Status::Corruption("bad block handle");
  }
  offset = off;
  size = sz;
  return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
  std::string handles;
  meta_index_handle.EncodeTo(&handles);
  index_handle.EncodeTo(&handles);
  dst->resize(kFooterSize, 0);
  if (!handles.empty()) {
    std::memcpy(dst->data(), handles.data(), handles.size());
  }
  EncodeFixed64(dst->data() + kFooterSize - kTableMagicNumberSize, kTableMagic);
}

Status Footer::DecodeFrom(const Slice& input, Footer* footer) {
  if (input.size() < kFooterSize) {
    return Status::Corruption("footer too short");
  }
  const char* magic = input.data() + kFooterSize - kTableMagicNumberSize;
  if (DecodeFixed64(magic) != kTableMagic) {
    return Status::Corruption("bad table magic number");
  }
  Slice rest(input.data(), kFooterSize - kTableMagicNumberSize);
  if (!footer->meta_index_handle.DecodeFrom(&rest).ok()) {
    return Status::Corruption("bad meta-index handle");
  }
  if (!footer->index_handle.DecodeFrom(&rest).ok()) {
    return Status::Corruption("bad index handle");
  }
  return Status::OK();
}

// ===========================================================================
// TableBuilder
// ===========================================================================

TableBuilder::TableBuilder(const InternalKeyComparator* icmp, WritableFile* file,
                           const FilterPolicy* filter_policy, size_t block_size)
    : icmp_(icmp),
      file_(file),
      filter_policy_(filter_policy),
      block_size_(block_size > 0 ? block_size : 4096),
      data_block_(1) {}

Status TableBuilder::WriteBlock(const Slice& block, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = block.size();
  Status s = file_->Append(block);
  if (s.ok()) {
    char trailer[kBlockTrailerSize];
    EncodeFixed32(trailer, crc32c::Value(block));
    trailer[4] = kBlockFullType;
    s = file_->Append(Slice(trailer, kBlockTrailerSize));
  }
  if (s.ok()) {
    offset_ += block.size() + kBlockTrailerSize;
  }
  return s;
}

Status TableBuilder::WriteRawBlock(const Slice& contents, BlockHandle* handle) {
  return WriteBlock(contents, handle);  // 过滤器已是最终字节，直接加 trailer 写入
}

void TableBuilder::Flush() {
  if (data_block_.empty()) return;
  WriteBlock(data_block_.Finish(), &pending_handle_);
  data_block_.Reset();
  pending_index_entry_ = true;
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  if (!status_.ok()) return;

  // 上一个 data block 已落盘，现在拿到了它的"下一个 key"，
  // 用它和 last_key_ 生成最短分隔键作为该 block 的索引键。
  if (pending_index_entry_) {
    icmp_->FindShortestSeparator(&last_key_, key);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  data_block_.Add(key, value);
  last_key_.assign(key.data(), key.size());
  if (smallest_key_.empty()) smallest_key_.assign(key.data(), key.size());
  largest_key_.assign(key.data(), key.size());

  if (filter_policy_ != nullptr) {
    filter_keys_.push_back(ExtractUserKey(key).ToString());
  }

  if (data_block_.CurrentSizeEstimate() >= block_size_) {
    Flush();
  }
}

Status TableBuilder::Finish() {
  Flush();  // 写完最后一个 data block

  if (pending_index_entry_) {
    // 最后一个 data block 的索引键：用 last_key_ 的最短严格后继
    icmp_->FindShortSuccessor(&last_key_);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  // 过滤器块
  BlockHandle filter_handle;
  if (filter_policy_ != nullptr && !filter_keys_.empty()) {
    std::vector<Slice> keys;
    keys.reserve(filter_keys_.size());
    for (const auto& k : filter_keys_) keys.emplace_back(k);
    std::string filter;
    filter_policy_->CreateFilter(keys.data(), static_cast<int>(keys.size()),
                                 &filter);
    WriteRawBlock(filter, &filter_handle);
  }

  // metaindex 块：把过滤器句柄登记到 "filter.<policy>"
  BlockBuilder meta_index_block(1);
  if (filter_policy_ != nullptr && !filter_keys_.empty()) {
    std::string key = "filter." + std::string(filter_policy_->Name());
    std::string handle_encoding;
    filter_handle.EncodeTo(&handle_encoding);
    meta_index_block.Add(Slice(key), Slice(handle_encoding));
  }
  BlockHandle meta_handle;
  WriteBlock(meta_index_block.Finish(), &meta_handle);

  // index 块
  BlockHandle index_handle;
  WriteBlock(index_block_.Finish(), &index_handle);

  // footer
  Footer footer;
  footer.meta_index_handle = meta_handle;
  footer.index_handle = index_handle;
  std::string footer_encoding;
  footer.EncodeTo(&footer_encoding);
  status_ = file_->Append(footer_encoding);
  if (status_.ok()) offset_ += footer_encoding.size();
  return status_;
}

// ===========================================================================
// Table
// ===========================================================================

Table::Table(const InternalKeyComparator* icmp,
             std::unique_ptr<RandomAccessFile> file, uint64_t file_size)
    : icmp_(icmp), file_(std::move(file)), file_size_(file_size) {}

Status Table::Open(const InternalKeyComparator* icmp,
                   std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                   Table** table) {
  if (file_size < kFooterSize) {
    return Status::Corruption("file is too short to be an sstable");
  }
  *table = nullptr;

  char footer_buf[kFooterSize];
  Slice footer;
  Status s = file->Read(file_size - kFooterSize, kFooterSize, &footer,
                        footer_buf);
  if (!s.ok()) return s;

  Footer f;
  s = Footer::DecodeFrom(footer, &f);
  if (!s.ok()) return s;

  Table* t = new Table(icmp, std::move(file), file_size);

  // 索引块常驻内存
  s = t->ReadBlock(f.index_handle, &t->index_contents_);
  if (!s.ok()) {
    delete t;
    return s;
  }
  t->index_block_ = std::make_unique<Block>(Slice(t->index_contents_));
  t->index_handle_ = f.index_handle;
  t->meta_index_handle_ = f.meta_index_handle;

  // metaindex -> 过滤器块
  std::string meta_contents;
  s = t->ReadBlock(f.meta_index_handle, &meta_contents);
  if (s.ok()) {
    Block meta_block{Slice(meta_contents)};
    Block::Iter mit(&meta_block, icmp->user_comparator());
    for (mit.SeekToFirst(); mit.Valid(); mit.Next()) {
      if (mit.key().starts_with("filter.")) {
        BlockHandle fh;
        Slice v = mit.value();
        if (fh.DecodeFrom(&v).ok()) {
          s = t->ReadBlock(fh, &t->filter_contents_);
          if (!s.ok()) {
            delete t;
            return s;
          }
        }
        break;
      }
    }
  }

  *table = t;
  return Status::OK();
}

Table::~Table() = default;

Status Table::ReadBlock(const BlockHandle& handle, std::string* contents) const {
  if (handle.offset + handle.size + kBlockTrailerSize > file_size_) {
    return Status::Corruption("read block past end of file");
  }
  std::string buf;
  buf.resize(handle.size + kBlockTrailerSize);
  Slice slice;
  Status s = file_->Read(handle.offset, buf.size(), &slice, buf.data());
  if (!s.ok()) return s;
  if (slice.size() != buf.size()) {
    return Status::Corruption("truncated block read");
  }
  // 校验块尾 crc（crc 只覆盖块内容本身，不含 trailer）
  const uint32_t stored = DecodeFixed32(slice.data() + handle.size);
  if (stored != crc32c::Value(Slice(slice.data(), handle.size))) {
    return Status::Corruption("block checksum mismatch");
  }
  contents->assign(slice.data(), handle.size);
  return Status::OK();
}

Status Table::Get(const Slice& user_key, SequenceNumber snapshot,
                  const FilterPolicy* filter_policy, std::string* value,
                  bool* found) const {
  *found = false;

  // 布隆过滤器短路：说"一定不在"就直接跳过整个文件
  if (filter_policy != nullptr && !filter_contents_.empty()) {
    if (!filter_policy->KeyMayMatch(user_key, Slice(filter_contents_))) {
      return Status::NotFound("table: bloom filter says not present");
    }
  }

  InternalKey lookup(user_key, snapshot, kTypeValue);
  const Slice lookup_key = lookup.Encode();

  // 1) 索引块定位候选 data block
  Block::Iter iter(index_block_.get(), icmp_);
  iter.Seek(lookup_key);
  if (!iter.Valid()) {
    return Status::NotFound("table: key not in index");
  }

  BlockHandle h;
  Slice hv = iter.value();
  if (!h.DecodeFrom(&hv).ok()) {
    return Status::Corruption("table: bad index entry");
  }

  // 2) 读出该 data block
  std::string data_contents;
  Status s = ReadBlock(h, &data_contents);
  if (!s.ok()) return s;

  // 3) 块内定位 + MVCC 可见性判断
  //
  // 比较器保证：同一个 user_key 的多个版本按 seq 降序（最新在前）排列。
  // 因此 Seek(lookup_key = user_key + pack(snapshot, kValueTypeForSeek)) 会落到
  // "第一个 internal_key >= lookup_key" 的位置——即第一个 seq <= snapshot 的版本
  // （所有 seq > snapshot 的版本都排在这个 lookup_key 之前，被 Seek 跳过）。
  // 落在点上之后仍需逐版本检查 seq <= snapshot，并对跨到别的 user_key 的情况判 NotFound。
  Block data_block{Slice(data_contents)};
  Block::Iter dit(&data_block, icmp_);
  dit.Seek(lookup_key);
  while (dit.Valid()) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(dit.key(), &parsed)) {
      return Status::Corruption("table: malformed internal key");
    }
    if (icmp_->user_comparator()->Compare(parsed.user_key, user_key) != 0) {
      // 已经跨到别的 user_key：本文件在该 snapshot 下没有这个 key 的版本
      break;
    }
    if (parsed.sequence <= snapshot) {
      // 找到了该快照下可见的版本
      *found = true;
      if (parsed.type == kTypeValue) {
        *value = dit.value().ToString();
        return Status::OK();
      }
      return Status::NotFound("table: key deleted");  // 可见删除
    }
    // seq > snapshot：更新的版本，对当前快照不可见，继续向后找更老的版本
    dit.Next();
  }
  return Status::NotFound("table: key not present at snapshot");
}

uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  // W4 简化实现：二分索引块找到"第一个 >= key 的 data block"，返回其偏移。
  // 精确估算留给 W5（Compaction 需要）。
  uint64_t result = 0;
  Block::Iter iter(index_block_.get(), icmp_);
  iter.Seek(key);
  if (iter.Valid()) {
    BlockHandle h;
    Slice hv = iter.value();
    if (h.DecodeFrom(&hv).ok()) result = h.offset;
  }
  return result;
}

}  // namespace tinystore
