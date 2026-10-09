#include "tinystore/write_batch.h"

#include "tinystore/coding.h"
#include "tinystore/memtable.h"  // WriteBatchInternal::InsertInto 需要 MemTable 完整定义

namespace tinystore {

// ===========================================================================
// WriteBatch 实现
// ===========================================================================

void WriteBatch::Put(const Slice& key, const Slice& value) {
  // 头部 count 自增（rep_ 至少 12 字节，偏移 8 起是 uint32 count）
  EncodeFixed32(rep_.data() + 8, DecodeFixed32(rep_.data() + 8) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  EncodeFixed32(rep_.data() + 8, DecodeFixed32(rep_.data() + 8) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(12);
  EncodeFixed64(rep_.data(), 0);  // sequence
  EncodeFixed32(rep_.data() + 8, 0);  // count
}

uint32_t WriteBatch::Count() const {
  if (rep_.size() < 12) return 0;
  return DecodeFixed32(rep_.data() + 8);
}

void WriteBatch::SetContents(const Slice& contents) {
  rep_.assign(contents.data(), contents.size());
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < 12) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(12);  // 跳过头部

  while (!input.empty()) {
    const char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue: {
        Slice key, value;
        if (!GetLengthPrefixedSlice(&input, &key) ||
            !GetLengthPrefixedSlice(&input, &value)) {
          return Status::Corruption("bad WriteBatch Put record");
        }
        handler->Put(key, value);
        break;
      }
      case kTypeDeletion: {
        Slice key;
        if (!GetLengthPrefixedSlice(&input, &key)) {
          return Status::Corruption("bad WriteBatch Delete record");
        }
        handler->Delete(key);
        break;
      }
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  return Status::OK();
}

// ===========================================================================
// WriteBatchInternal 实现
// ===========================================================================

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* batch) {
  if (batch->rep_.size() < 12) return 0;
  return DecodeFixed64(batch->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* batch, SequenceNumber seq) {
  batch->rep_.resize(std::max(batch->rep_.size(), static_cast<size_t>(12)));
  EncodeFixed64(batch->rep_.data(), seq);
}

Status WriteBatchInternal::InsertInto(const WriteBatch* batch,
                                     MemTable* memtable) {
  class Inserter : public WriteBatch::Handler {
  public:
    SequenceNumber seq = 0;
    MemTable* mem = nullptr;
    void Put(const Slice& k, const Slice& v) override {
      mem->Add(seq, kTypeValue, k, v);
      ++seq;
    }
    void Delete(const Slice& k) override {
      mem->Add(seq, kTypeDeletion, k, Slice());
      ++seq;
    }
  } ins;
  ins.seq = Sequence(batch);
  ins.mem = memtable;
  // 【必须检查 Iterate 的返回值】
  // Iterate 遇到非法格式返回 Corruption，此时一条都没插入。若忽略它继续推进
  // sequence，调用方（WAL 重放）会按头部 count 把 last_sequence_ 推到
  // 一个从未真正写入的区间 —— 产生 sequence 空洞，且这批数据静默丢失。
  Status s = batch->Iterate(&ins);
  if (!s.ok()) return s;
  // 把 batch 的起始 sequence 推进到最后一条之后，方便下一个 batch 续接
  SetSequence(const_cast<WriteBatch*>(batch), ins.seq);
  return Status::OK();
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  const uint32_t src_count = src->Count();
  const uint32_t dst_count = dst->Count();
  // 跳过 src 的 12 字节头部，把 record 段追加到 dst
  Slice input(src->rep_);
  input.remove_prefix(12);
  dst->rep_.append(input.data(), input.size());
  // 更新 dst 头部 count（rep_ 至少 12 字节，已保证）
  EncodeFixed32(dst->rep_.data() + 8, dst_count + src_count);
}

}  // namespace tinystore
