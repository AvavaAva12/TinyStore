#include "tinystore/memtable.h"

#include <cstring>

namespace tinystore {

// ===========================================================================
// MemTable 实现
// ===========================================================================

MemTable::MemTable(const InternalKeyComparator* comparator)
    : comparator_(comparator),
      table_(MemTableKeyComparator{comparator_}, &arena_),
      refs_(0) {}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  // 1) 把 (user_key, seq, type) 编码成 internal_key
  std::string ikey;
  AppendInternalKey(&ikey, ParsedInternalKey(key, seq, type));
  const size_t ik_size = ikey.size();
  const size_t val_size = value.size();

  // 2) 在 Arena 里分配这条记录的缓冲区：
  //    [varint(ik_size)][internal_key][varint(val_size)][value]
  //    删除记录的 val_size == 0，只有零长度前缀、没有 value 字节。
  const size_t record_size =
      VarintLength(ik_size) + ik_size + VarintLength(val_size) + val_size;
  char* const rec = arena_.AllocateAligned(record_size);
  char* p = EncodeVarint32(rec, static_cast<uint32_t>(ik_size));
  if (ik_size > 0) std::memcpy(p, ikey.data(), ik_size);
  char* q = p + ik_size;
  q = EncodeVarint32(q, static_cast<uint32_t>(val_size));
  if (val_size > 0) std::memcpy(q, value.data(), val_size);

  // 3) 把记录指针交给跳表（跳表负责分配节点并链入）
  std::lock_guard<std::mutex> lock(mu_);
  table_.Insert(rec);

  // 4) 累加实际数据量，供 ApproximateMemoryUsage() 判断何时 flush。
  //    放在锁外累加即可：fetch_add 自身原子，且只做统计、不影响正确性。
  data_size_.fetch_add(record_size, std::memory_order_relaxed);
}

Status MemTable::Get(const Slice& user_key, SequenceNumber snapshot,
                     std::string* value, bool* found) const {
  if (found != nullptr) *found = false;
  LookupKey lkey(user_key, snapshot);

  // 无锁读：跳表的 next 指针是 atomic（release 发布 / acquire 读取），
  // 读者顺着指针走不会被写者的插入撕裂。Arena 节点随 MemTable 一次性释放，
  // 且读期间通过外部 Ref 保证 MemTable 不被销毁，因此这里不需要任何互斥锁。
  // 这样快照读与前台写完全并行，不被 mu_ 阻塞。
  SkipList<const char*, MemTableKeyComparator>::Iterator iter(&table_);
  iter.Seek(lkey.data());
  if (!iter.Valid()) {
    return Status::NotFound("memtable: key not found");
  }

  // 取出 internal_key（用裸指针过载，避免按 strlen 构造 Slice 越界）
  const char* ptr = iter.key();
  Slice internal_key = GetLengthPrefixedSlice(ptr);
  // 注意：CompareUserKey 期望两个都是"内部键"；这里只比对 user_key 部分，
  // 否则把裸 user_key 当内部键传进去会在 ExtractUserKey 里断言失败。
  if (comparator_->user_comparator()->Compare(ExtractUserKey(internal_key),
                                               user_key) != 0) {
    // 落点已经不在同一个 user_key 上 -> 该 key 在本 MemTable 中不存在
    return Status::NotFound("memtable: key not found");
  }

  // 解析类型：墓碑 -> NotFound，有效值 -> 返回 value
  ParsedInternalKey parsed;
  if (!ParseInternalKey(internal_key, &parsed)) {
    return Status::Corruption("memtable: malformed internal key");
  }
  if (parsed.type == kTypeDeletion) {
    if (found != nullptr) *found = true;  // key 在 memtable，但最新可见是删除
    return Status::NotFound("memtable: key deleted");
  }

  // value 紧跟在 internal_key 之后：[varint(val_size)][value]
  const char* val_ptr = internal_key.data() + internal_key.size();
  Slice val = GetLengthPrefixedSlice(val_ptr);
  *value = val.ToString();
  if (found != nullptr) *found = true;  // key 确实在 memtable（有效值）
  return Status::OK();
}

// ===========================================================================
// MemTable::Iterator 实现
// ===========================================================================

MemTable::Iterator::Iterator(const MemTable* mem)
    : iter_(&mem->table_), mem_(mem) {}

bool MemTable::Iterator::Valid() const { return iter_.Valid(); }

void MemTable::Iterator::SeekToFirst() { iter_.SeekToFirst(); }
void MemTable::Iterator::SeekToLast() { iter_.SeekToLast(); }
void MemTable::Iterator::Next() { iter_.Next(); }
void MemTable::Iterator::Prev() { iter_.Prev(); }

void MemTable::Iterator::Seek(const Slice& user_key) {
  LookupKey lk(user_key, kMaxSequenceNumber);
  iter_.Seek(lk.data());
}

Slice MemTable::Iterator::internal_key() const {
  return GetLengthPrefixedSlice(iter_.key());
}

Slice MemTable::Iterator::user_key() const {
  return ExtractUserKey(internal_key());
}

Slice MemTable::Iterator::value() const {
  Slice ik = internal_key();
  const char* val_ptr = ik.data() + ik.size();
  return GetLengthPrefixedSlice(val_ptr);
}

SequenceNumber MemTable::Iterator::sequence() const {
  ParsedInternalKey parsed;
  if (ParseInternalKey(internal_key(), &parsed)) return parsed.sequence;
  return 0;
}

ValueType MemTable::Iterator::type() const {
  ParsedInternalKey parsed;
  if (ParseInternalKey(internal_key(), &parsed)) return parsed.type;
  return kTypeValue;
}

}  // namespace tinystore
