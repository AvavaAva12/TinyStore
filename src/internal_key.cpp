#include "tinystore/internal_key.h"

#include <cassert>
#include <string>

namespace tinystore {

std::string ParsedInternalKey::DebugString() const {
    return "'" + user_key.ToString() + "' @ " + std::to_string(sequence) + " : " +
           std::to_string(static_cast<int>(type));
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
    result->append(key.user_key.data(), key.user_key.size());
    PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
    // 长度至少要有 8 字节后缀
    if (internal_key.size() < kInternalKeySuffixSize) {
        return false;
    }

    const size_t user_key_size = internal_key.size() - kInternalKeySuffixSize;
    const uint64_t packed = DecodeFixed64(internal_key.data() + user_key_size);

    const auto type_byte = static_cast<uint8_t>(packed & 0xff);
    const SequenceNumber sequence = packed >> 8;

    // 校验 type 字段。遇到未知值时拒绝解析，而不是塞一个非法值给上层。
    // 这是"fail fast"原则：让数据损坏在解析层就被发现，
    // 而不是等它传播到 Compaction 逻辑里变成难以理解的行为。
    if (type_byte > kTypeValue) {
        return false;
    }

    result->user_key = Slice(internal_key.data(), user_key_size);
    result->sequence = sequence;
    result->type = static_cast<ValueType>(type_byte);
    return true;
}

int InternalKeyComparator::Compare(const Slice& a, const Slice& b) const {
    // 第一优先级：user_key 升序。
    // 这里用用户比较器（默认字节序），完全不涉及后缀。
    int r = user_comparator_->Compare(ExtractUserKey(a), ExtractUserKey(b));
    if (r != 0) {
        return r;
    }

    // 第二优先级：sequence 降序（新的在前）。
    //
    // 必须先 DecodeFixed64 解成数值，绝不能直接 memcmp 后缀！
    // 原因见头文件里的详细推导（小端序下 memcmp 与数值序不一致）。
    const uint64_t a_packed = DecodeFixed64(a.data() + a.size() -
                                            kInternalKeySuffixSize);
    const uint64_t b_packed = DecodeFixed64(b.data() + b.size() -
                                            kInternalKeySuffixSize);

    if (a_packed > b_packed) {
        r = -1;   // a 的 sequence 更大 = 更新 = 排在前面
    } else if (a_packed < b_packed) {
        r = +1;
    }
    // a_packed == b_packed 时 r 保持 0（同一个键的同一版本）
    return r;
}

const char* InternalKeyComparator::Name() const {
    return "tinystore.InternalKeyComparator";
}

std::string InternalKey::DebugString() const {
    ParsedInternalKey parsed;
    if (ParseInternalKey(rep_, &parsed)) {
        return parsed.DebugString();
    }
    // 无法解析时输出转义形式，便于调试损坏数据
    std::string result = "(bad)";
    for (char c : rep_) {
        result.push_back(c == '\0' ? '\\' : c);
    }
    return result;
}

}  // namespace tinystore
