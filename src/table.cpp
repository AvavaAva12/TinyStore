#include "tinystore/table.h"

#include <cassert>
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
    // 两个 BlockHandle 各至多 2 个 varint64 = 20 字节，合计 40 字节，
    // 恰好等于 footer 里留给句柄的空间（kFooterSize - kTableMagicNumberSize）。
    // 当前是安全的，但依赖的是"varint64 最长 10 字节"这个编码细节——
    // 用断言把这个不变量显式固定下来，万一编码方式变化会立刻暴露，
    // 而不是变成一次静默的 memcpy 越界写。
    assert(handles.size() <= kFooterSize - kTableMagicNumberSize);
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

Status TableBuilder::Flush() {
  if (data_block_.empty()) return Status::OK();
  Status s = WriteBlock(data_block_.Finish(), &pending_handle_);
  if (!s.ok()) {
    // 落盘失败：把错误记入 status_，让整个 TableBuilder 作废。
    // 关键在于「失败的文件绝不能被当成构建成功」——否则上层会把一个
    // 缺块/截断的 SSTable 登记进 MANIFEST 并删掉旧 WAL，数据静默丢失。
    status_ = s;
    return s;
  }
  data_block_.Reset();
  pending_index_entry_ = true;
  return Status::OK();
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
  ++num_entries_;
  if (smallest_key_.empty()) smallest_key_.assign(key.data(), key.size());
  largest_key_.assign(key.data(), key.size());

  if (filter_policy_ != nullptr) {
    filter_keys_.push_back(ExtractUserKey(key).ToString());
  }

  if (data_block_.CurrentSizeEstimate() >= block_size_) {
    // 错误已记入 status_，此处提前返回；后续 Add 会被开头的 status_ 检查挡住。
    if (!Flush().ok()) return;
  }
}

Status TableBuilder::Finish() {
  // 【错误汇聚原则】status_ 是唯一的错误汇聚点：任何一次写入失败都立刻记入
  // status_ 并返回，后续 Add() 开头与本函数开头的检查会保证不再产生任何写入。
  // 这样"构建失败"与"文件内容残缺"就不可能同时出现——上层拿到非 OK 状态时，
  // 文件里可能是旧数据，但绝不会是一个被误认为完整的 SSTable。
  if (!status_.ok()) return status_;

  Status s = Flush();  // 写完最后一个 data block
  if (!s.ok()) return s;

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
    s = WriteRawBlock(filter, &filter_handle);
    if (!s.ok()) {
      status_ = s;
      return s;
    }
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
  s = WriteBlock(meta_index_block.Finish(), &meta_handle);
  if (!s.ok()) {
    status_ = s;
    return s;
  }

  // index 块
  BlockHandle index_handle;
  s = WriteBlock(index_block_.Finish(), &index_handle);
  if (!s.ok()) {
    status_ = s;
    return s;
  }

  // footer
  Footer footer;
  footer.meta_index_handle = meta_handle;
  footer.index_handle = index_handle;
  std::string footer_encoding;
  footer.EncodeTo(&footer_encoding);
  s = file_->Append(footer_encoding);
  if (s.ok()) {
    offset_ += footer_encoding.size();
  } else {
    status_ = s;
  }
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
  // 【为什么不能写成 offset + size + trailer > file_size_】
  // handle 来自磁盘上的 footer，属于不可信输入。三项相加在 uint64 里会回绕：
  // 一个接近 UINT64_MAX 的 offset 加上 size 会绕回成小数，让这个检查形同虚设，
  // 随后就带着超大 offset 发起 pread。这里改成分步的无溢出比较，
  // 任何异常大的值都会在第一步（offset 越界）或第二步（size 越界/放不下 trailer）被拦下。
  if (handle.offset > file_size_) {
    return Status::Corruption("read block past end of file");
  }
  const uint64_t remaining = file_size_ - handle.offset;
  // 先判 size <= remaining 再做减法，避免 remaining - handle.size 自身下溢。
  if (handle.size > remaining || remaining - handle.size < kBlockTrailerSize) {
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

// ===========================================================================
// TableIterator —— 单个 SSTable 的顺序遍历
// ===========================================================================
namespace detail {

// 【遍历结构：索引块常驻，数据块按需预读】
// index block 只有几百字节，整个常驻内存，遍历时从头到尾走一遍它，
// 每条 entry 给出一个 data block 的句柄。每跨一个 entry 就 pread 那一个
// data block 到 data_contents_，用完直接被下一个覆盖——所以内存占用
// 与文件大小无关，恒定为"一个块 + 一个索引块"。
//
// 【为什么 index_iter_.Seek(target) 能定位到正确的块】
// 索引 entry 的 key 是"该块上界与下一块下界之间的最短分隔键"，
// 它严格大于本块所有实际 key。于是"第一个索引 key >= target 的 entry"
// 恰好就是包含 target 的那个块：
//   target 落在块内        -> 分隔键 >= target，命中本块
//   target 是本块最后一个  -> 分隔键 > target，仍命中本块
//   target 超过全部数据    -> 没有 entry >= target，Valid() 为假，遍历正确结束
class TableIterator : public Iterator {
public:
  explicit TableIterator(const Table* table)
      : table_(table), index_iter_(table->index_block_.get(), table->icmp_) {}

  bool Valid() const override { return valid_ && data_iter_ && data_iter_->Valid(); }

  void SeekToFirst() override {
    if (!status_.ok()) return;
    index_iter_.SeekToFirst();
    if (!index_iter_.Valid()) {  // 空表：index block 里没有任何 entry
      valid_ = false;
      return;
    }
    LoadCurrentBlock();                       // 进入第一个 data block
    if (!data_iter_) return;
    data_iter_->SeekToFirst();                // 块内从头开始
    valid_ = data_iter_->Valid();
  }

  void SeekToLast() override {
    if (!status_.ok()) return;
    index_iter_.SeekToLast();
    if (!index_iter_.Valid()) {  // 空表
      valid_ = false;
      return;
    }
    LoadCurrentBlock();
    if (!data_iter_) return;
    data_iter_->SeekToLast();                 // 块内从尾开始
    valid_ = data_iter_->Valid();
  }

  // 按 user_key 定位到它的最新版本。
  //
  // 对外约定 target 是裸 user_key（与 Get 一致），但块内查找用的是完整
  // internal_key，所以这里构造一个查找键：user_key 配上 (kMaxSequenceNumber,
  // kTypeValue)。比较器对同 user_key 按 seq 降序排，这个后缀排在所有真实版本
  // 之前，于是 Seek 恰好停在"该 user_key 的第一个（最新）版本"上。
  void Seek(const Slice& target) override {
    if (!status_.ok()) return;
    const InternalKey lookup(target, kMaxSequenceNumber, kTypeValue);
    const Slice lookup_ikey = lookup.Encode();
    // 先在索引里定位到候选块，再在块内二分。
    index_iter_.Seek(lookup_ikey);
    if (!index_iter_.Valid()) {  // target 大于文件里所有 key
      valid_ = false;
      return;
    }
    LoadCurrentBlock();
    if (!data_iter_) return;
    data_iter_->Seek(lookup_ikey);             // 块内找第一个 >= 查找键的 entry
    valid_ = data_iter_->Valid();
  }

  void Next() override {
    if (!Valid()) return;
    data_iter_->Next();
    while (data_iter_ && !data_iter_->Valid()) {
      // 当前块扫完，进入下一个块（index entry）。
      if (!NextIndexEntry()) return;
      LoadCurrentBlock();
      if (!data_iter_) return;
      data_iter_->SeekToFirst();              // 新块从头开始
    }
    valid_ = data_iter_ && data_iter_->Valid();
  }

  // 反向跨块：与 Next 完全对称，只是 index 迭代器往回走、数据块从尾开始。
  //
  // 【为什么不需要额外记录"当前在第几个块"】
  // index_iter_ 自己就是当前位置的权威（它正停在产生当前 data block 的那个
  // index entry 上），Block::Iter::Prev 在块内退回无效时只需让 index_iter_
  // 再退一条，语义天然对齐。
  void Prev() override {
    if (!Valid()) return;
    data_iter_->Prev();
    while (data_iter_ && !data_iter_->Valid()) {
      // 当前块退到头，进入上一个块。
      if (!PrevIndexEntry()) return;
      LoadCurrentBlock();
      if (!data_iter_) return;
      data_iter_->SeekToLast();               // 新块从尾开始
    }
    valid_ = data_iter_ && data_iter_->Valid();
  }

  Slice key() const override {
    assert(Valid());
    return data_iter_->key();
  }
  Slice value() const override {
    assert(Valid());
    return data_iter_->value();
  }
  Status status() const override { return status_; }

private:
  // 把 index_iter_ 当前 entry 的句柄读出来，加载成当前 data block。
  void LoadCurrentBlock() {
    BlockHandle h;
    Slice hv = index_iter_.value();
    Status s = h.DecodeFrom(&hv);
    if (!s.ok()) {
      status_ = s;
      valid_ = false;
      return;
    }
    s = table_->ReadBlock(h, &data_contents_);
    if (!s.ok()) {
      status_ = s;
      valid_ = false;
      return;
    }
    // Block 存进 unique_ptr 而非栈上对象：Block::Iter 内部持有指向 Block 的
    // 裸指针，必须保证 Block 的地址在 data_iter_ 存活期间稳定不变。
    data_block_ = std::make_unique<Block>(Slice(data_contents_));
    data_iter_ = std::make_unique<Block::Iter>(data_block_.get(), table_->icmp_);
  }

  // index_iter_ 前进到下一个 entry；返回是否还有下一个块。
  bool NextIndexEntry() {
    index_iter_.Next();
    if (!index_iter_.Valid()) {
      valid_ = false;  // 正常遍历到文件末尾
      return false;
    }
    return true;
  }

  bool PrevIndexEntry() {
    index_iter_.Prev();
    if (!index_iter_.Valid()) {
      valid_ = false;  // 正常遍历到文件开头
      return false;
    }
    return true;
  }

  const Table* table_;
  Block::Iter index_iter_;
  std::string data_contents_;                 // 当前 data block 的内容
  std::unique_ptr<Block> data_block_;
  std::unique_ptr<Block::Iter> data_iter_;
  bool valid_ = false;
  Status status_;
};

}  // namespace detail

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<detail::TableIterator>(this);
}

}  // namespace tinystore
