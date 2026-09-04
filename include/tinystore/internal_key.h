#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "tinystore/coding.h"
#include "tinystore/comparator.h"
#include "tinystore/slice.h"

namespace tinystore {

// ===========================================================================
// 内部键（InternalKey）编码
//
// 这是整个存储引擎最重要的一个设计决策，务必彻底理解。
// ===========================================================================
//
// 【为什么需要它】
// 用户看到的 key 是 "name"、"user:1001" 这样的裸字节串。
// 但引擎内部必须能区分"同一个 key 的不同版本"：
//
//     t1: Put("a", "v1")     sequence = 10
//     t2: Put("a", "v2")     sequence = 20
//     t3: Delete("a")        sequence = 30
//
// 三个操作都作用在 "a" 上。为了让它们能在同一个有序结构里共存，
// 并且让"读最新值"变成一次简单的有序查找，我们把版本号编码进 key 本身：
//
//     InternalKey = | user_key (变长) | sequence (7 字节) | type (1 字节) |
//                                     └──────── 固定 8 字节后缀 ────────┘
//
// sequence: 全局单调递增的操作序号，越大表示越"新"。
// type:     kTypeValue（有效值）或 kTypeDeletion（墓碑标记）。
//
// 【排序规则（核心）】
// 定义了上面的编码后，比较两个 InternalKey 的规则是：
//
//     1. 先按 user_key 升序（用用户的 Comparator，默认字节序）
//     2. user_key 相同时，按 sequence **降序**（新版本排前面）
//
// 这个规则带来极其重要的性质 —— 对某个 user_key 做查找时，
// 在有序结构里**遇到的第一个匹配项就是最新版本**，可以立即停止扫描。
//
// 这就是 MVCC 快照读的实现基础：
//     "读 sequence <= 100 时刻的值"  <=>  "找到第一个 seq <= 100 的项"
// 如果没有把 sequence 编码进 key，就只能遍历该 key 的所有版本再过滤，
// 等于没有 MVCC。
//
// 【为什么 sequence 只用 7 字节】
// 8 字节后缀里，低 1 字节给了 type，剩下 7 字节（56 位）给 sequence。
// 56 位能表示约 7.2e16 个序号。即使每秒写入 10 亿条，
// 也要用掉约 228 年才耗尽，实际永远用不完。
// 换来的是：type 可以免费搭车，不需要额外字节。
//
// 【为什么 suffix 必须用数值比较，而不能整串 memcmp】
// 这是一个非常隐蔽的陷阱，也是面试高频追问点。
//
// suffix 里存的是 (sequence << 8) | type，按**小端序**编码为 64 位。
// 小端序意味着"最低有效字节在最低地址"，而 memcmp 是从低地址开始比的。
// 举个反例：
//
//     A = 0x0100 (256)  小端字节序: [00][01]
//     B = 0x00FF (255)  小端字节序: [FF][00]
//
//     memcmp  比较: 第一个字节 0x00 < 0xFF  ->  得出 A < B   （错误！）
//     数值比较:     256 > 255              ->  得出 A > B   （正确）
//
// 所以正确做法是：user_key 部分用 memcmp（字节序比较没问题），
// 后缀部分必须 DecodeFixed64 解成数值再比，然后取反号实现降序。
// 混用两者会产生极难排查的排序错乱。
// ===========================================================================

// 操作序列号类型
using SequenceNumber = uint64_t;

// 内部键的类型标记。
// 显式指定底层类型为 uint8_t，这样任何 uint8_t 值都是合法的枚举值，
// 解析损坏数据时不会因为 static_cast 产生未定义行为。
enum ValueType : uint8_t {
    kTypeDeletion = 0x0,   // 墓碑（tombstone）：标记删除，Compaction 时才真正清除
    kTypeValue = 0x1,      // 正常的键值写入
};

// sequence 可用的最大值（56 位全 1）
constexpr SequenceNumber kMaxSequenceNumber =
    (static_cast<uint64_t>(1) << 56) - 1;

// 后缀固定 8 字节
constexpr size_t kInternalKeySuffixSize = 8;

// ---------------------------------------------------------------------------
// 解析后的内部键
// ---------------------------------------------------------------------------
struct ParsedInternalKey {
    Slice user_key;
    SequenceNumber sequence;
    ValueType type;

    ParsedInternalKey() : sequence(kMaxSequenceNumber), type(kTypeValue) {}

    ParsedInternalKey(const Slice& u, const SequenceNumber& s, ValueType t)
        : user_key(u), sequence(s), type(t) {}

    std::string DebugString() const;
};

// 把 (sequence, type) 打包成一个 64 位整数：高 56 位是 sequence，低 8 位是 type
inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
    // seq 超过 56 位会覆盖 type 字段，属于编程错误
    assert(seq <= kMaxSequenceNumber);
    return (seq << 8) | static_cast<uint8_t>(type);
}

// 把 ParsedInternalKey 追加序列化到 result 尾部。
// 用 append 而非返回新 string：热路径上可以复用同一个缓冲区，避免反复分配。
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// 解析内部键。
//
// 返回 false 表示数据非法（长度不足，或 type 字段是未知值）——
// 这通常意味着磁盘数据损坏，调用方应返回 Corruption 而不是崩溃。
//
// 注意：成功时 result->user_key 是指向 internal_key 内部缓冲区的 Slice，
// 调用方必须保证 internal_key 的底层内存比 result 活得更久。
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// 截取 user_key 部分（去掉尾部 8 字节后缀）
inline Slice ExtractUserKey(const Slice& internal_key) {
    assert(internal_key.size() >= kInternalKeySuffixSize);
    return Slice(internal_key.data(),
                 internal_key.size() - kInternalKeySuffixSize);
}

// ---------------------------------------------------------------------------
// InternalKey —— 持有自身存储的内部键
//
// 与 ParsedInternalKey 的区别：
//   * ParsedInternalKey 是"视图"，指向别人的内存，零拷贝但需要小心生命周期；
//   * InternalKey 自己持有一份 std::string，可安全传递和保存。
// 两者按需选用：查找时用前者避免拷贝，需要保存时用后者。
// ---------------------------------------------------------------------------
class InternalKey {
public:
    InternalKey() = default;

    InternalKey(const Slice& user_key, SequenceNumber seq, ValueType type) {
        AppendInternalKey(&rep_, ParsedInternalKey(user_key, seq, type));
    }

    // 从已编码的字节串构造（会拷贝一份）
    explicit InternalKey(const Slice& encoded)
        : rep_(encoded.data(), encoded.size()) {}

    void DecodeFrom(const Slice& encoded) {
        rep_.assign(encoded.data(), encoded.size());
    }

    void SetFrom(const ParsedInternalKey& p) {
        rep_.clear();
        AppendInternalKey(&rep_, p);
    }

    void Clear() { rep_.clear(); }

    // 注意：返回的 Slice 指向 rep_ 内部，rep_ 变化或对象析构后即失效。
    Slice Encode() const { return rep_; }

    Slice user_key() const { return ExtractUserKey(rep_); }

    bool valid() const { return rep_.size() >= kInternalKeySuffixSize; }

    std::string DebugString() const;

private:
    std::string rep_;
};

// ---------------------------------------------------------------------------
// InternalKeyComparator —— 内部键的比较器
//
// 它**包装**了用户的 Comparator，在其之上叠加版本号语义。
// 这是"装饰器模式"在系统编程中的一个典型应用：
// 用户只关心 user_key 怎么比，引擎负责在其之上叠加 sequence 的规则。
// ---------------------------------------------------------------------------
class InternalKeyComparator : public Comparator {
public:
    explicit InternalKeyComparator(const Comparator* user_comparator)
        : user_comparator_(user_comparator) {}

    const Comparator* user_comparator() const { return user_comparator_; }

    int Compare(const Slice& a, const Slice& b) const;

    int Compare(const InternalKey& a, const InternalKey& b) const {
        return Compare(a.Encode(), b.Encode());
    }

    // 只比较 user_key 部分，忽略 sequence。
    // 用途：Compaction 判断两个内部键是否属于同一个用户键；
    //       以及判断某个 key 是否落在某个 SSTable 的 [smallest, largest] 区间内。
    int CompareUserKey(const Slice& a, const Slice& b) const {
        return user_comparator_->Compare(ExtractUserKey(a), ExtractUserKey(b));
    }

    // 用于持久化到 SSTable 元数据，防止用错比较器打开数据库
    const char* Name() const;
    // 分隔键 / 后继键只能压缩 user_key 部分，绝不能拿整个 InternalKey 去调
    // user_comparator_（会把 8 字节后缀一起压缩，产生长度 < 8 的非法 InternalKey，
    // 读路径解码后缀时 size - 8 下溢、Slice 越界 → 段错误）。压缩后再用最大
    // (seq, type) 拼回合法 InternalKey，使其排在原 user_key 所有版本之后、小于下一 user_key。
    void FindShortestSeparator(std::string* start, const Slice& limit) const override {
        Slice user_start = ExtractUserKey(*start);
        Slice user_limit = ExtractUserKey(limit);
        std::string tmp(user_start.data(), user_start.size());
        user_comparator_->FindShortestSeparator(&tmp, user_limit);
        if (tmp.size() < user_start.size() &&
            user_comparator_->Compare(user_start, Slice(tmp)) < 0) {
            *start = InternalKey(tmp, kMaxSequenceNumber, kTypeValue).Encode().ToString();
        }
    }
    void FindShortSuccessor(std::string* key) const override {
        Slice user_key = ExtractUserKey(*key);
        std::string tmp(user_key.data(), user_key.size());
        user_comparator_->FindShortSuccessor(&tmp);
        if (tmp.size() < user_key.size() &&
            user_comparator_->Compare(user_key, Slice(tmp)) < 0) {
            *key = InternalKey(tmp, kMaxSequenceNumber, kTypeValue).Encode().ToString();
        }
    }

private:
    const Comparator* user_comparator_;
};

}  // namespace tinystore
