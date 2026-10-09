#include "tinystore/block.h"

#include <cassert>
#include <cstring>

#include "tinystore/coding.h"
#include "tinystore/crc32c.h"

namespace tinystore {
namespace {

// 块尾 5 字节：crc32c(4B, 小端) + type(1B)
constexpr size_t kBlockTrailerSize = 5;
// 块尾 type 字节：W4 只写完整的块，固定为 1（与 WAL 的 kFullType 含义一致）
constexpr uint8_t kBlockFullType = 1;

}  // namespace

// ===========================================================================
// BlockBuilder
// ===========================================================================

BlockBuilder::BlockBuilder(int block_restart_interval)
    : block_restart_interval_(block_restart_interval) {
  assert(block_restart_interval_ > 0);
  Reset();
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // 第一个重启点固定在偏移 0
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  // 调用方（MemTable 迭代器）保证 key 严格递增，这里不再重复校验以省开销。

  Slice last(last_key_);
  size_t shared = 0;
  if (counter_ < block_restart_interval_) {
    // 与 last_key_ 求公共前缀（原始字节比较即可：前缀压缩只是无损编码优化，
    // 不影响排序——排序由 Block 读取时用的 Comparator 决定）。
    const size_t min_len = std::min(last.size(), key.size());
    while (shared < min_len && last[shared] == key[shared]) ++shared;
  } else {
    // 达到重启间隔：开新重启点，shared = 0（完整存 key）
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  const size_t non_shared = key.size() - shared;
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++counter_;
}

Slice BlockBuilder::Finish() {
  // 追加重启点数组 + 个数
  for (uint32_t r : restarts_) PutFixed32(&buffer_, r);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size();
}

// ===========================================================================
// Block（只读）
// ===========================================================================

Block::Block(const Slice& contents)
    : data_(contents.data()), size_(contents.size()), num_restarts_(0),
      restarts_ptr_(contents.data()) {
  if (size_ >= kBlockTrailerSize + 4) {
    num_restarts_ =
        DecodeFixed32(data_ + size_ - 4);
    restarts_ptr_ = data_ + size_ - 4 -
                    4 * static_cast<size_t>(num_restarts_);
    entries_end_ = static_cast<uint32_t>(restarts_ptr_ - data_);
  }
}

Status Block::Get(const Comparator* cmp, const Slice& lookup,
                  std::string* key, std::string* value) const {
  Iter iter(this, cmp);
  iter.Seek(lookup);
  if (iter.Valid()) {
    *key = iter.key().ToString();
    *value = iter.value().ToString();
    return Status::OK();
  }
  return Status::NotFound("block: key not found");
}

// ---------------------------------------------------------------------------
// Block::Iter
// ---------------------------------------------------------------------------

Block::Iter::Iter(const Block* block, const Comparator* cmp)
    : block_(block), cmp_(cmp) {}

bool Block::Iter::ParseEntry(uint32_t offset, const std::string& prev_key,
                             std::string* out_key, std::string* out_value,
                             uint32_t* next_offset) {
  const char* limit = block_->data_ + block_->entries_end_;
  if (offset >= block_->entries_end_) return false;

  const char* p = block_->data_ + offset;
  uint32_t shared, non_shared, value_len;
  p = GetVarint32Ptr(p, limit, &shared);
  if (p == nullptr) return false;
  p = GetVarint32Ptr(p, limit, &non_shared);
  if (p == nullptr) return false;
  p = GetVarint32Ptr(p, limit, &value_len);
  if (p == nullptr) return false;
  if (static_cast<size_t>(p - block_->data_) + non_shared + value_len >
      block_->entries_end_) {
    return false;  // 越界：块损坏
  }

  // 还原完整 key = prev_key 的前 shared 字节 + 非共享字节
  assert(shared <= prev_key.size());
  out_key->assign(prev_key.data(), shared);
  out_key->append(p, non_shared);
  p += non_shared;
  out_value->assign(p, value_len);

  *next_offset = static_cast<uint32_t>((p + value_len) - block_->data_);
  return true;
}

void Block::Iter::SeekToFirst() {
  if (block_->num_restarts_ == 0) {
    valid_ = false;
    return;
  }
  restart_index_ = 0;
  current_ = DecodeFixed32(block_->restarts_ptr_);
  std::string prev;
  uint32_t next;
  valid_ = ParseEntry(current_, prev, &key_, &value_, &next);
  next_offset_ = next;
}

void Block::Iter::Seek(const Slice& target) {
  if (block_->num_restarts_ == 0) {
    valid_ = false;
    return;
  }
  // 二分查找：第一个 key >= target 的重启点记为 left；
  // 我们需要的是“最后一个 key <= target 的重启点” = left - 1（越界则取 0）。
  // 注意 mid 必须落在 [0, num_restarts_-1]，否则会越界读到重启计数。
  uint32_t left = 0, right = block_->num_restarts_;
  while (left < right) {
    const uint32_t mid = left + (right - left) / 2;  // mid ∈ [left, right)
    const uint32_t offset = DecodeFixed32(block_->restarts_ptr_ + 4 * mid);
    std::string k, v;
    uint32_t next;
    if (!ParseEntry(offset, std::string(), &k, &v, &next)) {
      valid_ = false;
      return;
    }
    if (cmp_->Compare(Slice(k), target) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  const uint32_t restart = (left == 0) ? 0 : left - 1;

  // 从重启点 restart 线性扫描，直到第一个 key >= target
  uint32_t offset = DecodeFixed32(block_->restarts_ptr_ + 4 * restart);
  std::string prev;
  uint32_t next;
  while (offset < block_->entries_end_) {
    std::string k, v;
    if (!ParseEntry(offset, prev, &k, &v, &next)) {
      valid_ = false;
      return;
    }
    prev = k;
    if (cmp_->Compare(Slice(k), target) >= 0) {
      key_ = k;
      value_ = v;
      current_ = offset;
      next_offset_ = next;
      valid_ = true;
      return;
    }
    offset = next;
  }
  valid_ = false;
}

void Block::Iter::Next() {
  if (!valid_) return;
  if (next_offset_ >= block_->entries_end_) {
    valid_ = false;
    return;
  }
  std::string prev = key_;
  uint32_t next;
  valid_ = ParseEntry(next_offset_, prev, &key_, &value_, &next);
  if (valid_) {
    current_ = next_offset_;
    next_offset_ = next;
  }
}

bool Block::Iter::FindLastBefore(uint32_t start_off, uint32_t limit_off,
                                 uint32_t* out_off, std::string* out_key,
                                 std::string* out_value, uint32_t* out_next) {
  if (start_off >= limit_off) return false;
  uint32_t off = start_off;
  std::string prev_key;
  uint32_t next = start_off;
  bool found = false;
  while (off < limit_off) {
    std::string k, v;
    if (!ParseEntry(off, prev_key, &k, &v, &next)) return false;  // 块损坏
    prev_key = k;
    *out_off = off;
    out_key->assign(k);
    out_value->assign(v);
    *out_next = next;
    found = true;
    off = next;
  }
  return found;
}

uint32_t Block::Iter::RestartIndexFor(uint32_t offset) const {
  // 不变式：restarts_[0] == 0，所以 lo 初始一定满足 restarts[lo] <= offset。
  uint32_t lo = 0, hi = block_->num_restarts_;
  while (lo + 1 < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (DecodeFixed32(block_->restarts_ptr_ + 4 * mid) <= offset) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void Block::Iter::SeekToLast() {
  if (block_->num_restarts_ == 0) {
    valid_ = false;
    return;
  }
  // 最后一个重启点所在区间就是块尾区间，扫到 entries_end 即为最后一条。
  restart_index_ = block_->num_restarts_ - 1;
  const uint32_t start = DecodeFixed32(block_->restarts_ptr_ + 4 * restart_index_);
  uint32_t off = 0, next = 0;
  std::string k, v;
  if (!FindLastBefore(start, block_->entries_end_, &off, &k, &v, &next)) {
    valid_ = false;
    return;
  }
  current_ = off;
  key_ = k;
  value_ = v;
  next_offset_ = next;
  valid_ = true;
}

void Block::Iter::Prev() {
  if (!valid_) return;

  // 1) 定位当前 entry 落在哪个重启点区间
  const uint32_t ri = RestartIndexFor(current_);
  const uint32_t start = DecodeFixed32(block_->restarts_ptr_ + 4 * ri);

  uint32_t off = 0, next = 0;
  std::string k, v;

  // 2) 同区间内向前找（current_ 不在区间首时成立）
  if (FindLastBefore(start, current_, &off, &k, &v, &next)) {
    current_ = off;
    key_ = k;
    value_ = v;
    next_offset_ = next;
    return;  // valid_ 保持 true
  }

  // 3) current_ 就是该区间的第一条 —— 前一个 entry 在**上一个区间**里。
  //    若它已经是块里第一条，则没有前驱，遍历结束。
  if (ri == 0) {
    valid_ = false;
    return;
  }
  const uint32_t prev_start = DecodeFixed32(block_->restarts_ptr_ + 4 * (ri - 1));
  const uint32_t prev_end = start;  // == restarts[ri]
  if (!FindLastBefore(prev_start, prev_end, &off, &k, &v, &next)) {
    valid_ = false;
    return;
  }
  restart_index_ = ri - 1;
  current_ = off;
  key_ = k;
  value_ = v;
  next_offset_ = next;
}

// cstdint / vector 已在头文件包含；这里补一条避免某些编译单元的未用告警。
static_assert(kBlockTrailerSize == 5, "block trailer size");

}  // namespace tinystore
