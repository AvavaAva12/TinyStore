#pragma once

#include <string>

#include "tinystore/slice.h"

namespace tinystore {

// ===========================================================================
// Comparator —— key 的排序规则抽象
// ===========================================================================
//
// 【为什么要有这一层抽象】
// 存储引擎的一切都建立在"key 有序"之上：MemTable 是有序结构、SSTable 内部
// 有序、Compaction 归并依赖有序。把"怎么比较两个 key"抽象出来后：
//   * 用户可以自定义排序（比如按整数数值序，而非字节序）；
//   * 内部键（InternalKey）的比较器可以包装用户比较器，叠加版本号语义。
//
// 【为什么 Name() 很重要】
// 磁盘上的 SSTable 是不可变的。如果用户用 A 比较器写入、用 B 比较器打开，
// 读取结果会完全错乱。Name() 会被持久化到 SSTable 的元数据里，
// 打开时校验，不匹配直接拒绝 —— 这是一种"以崩溃代替静默错误"的防御设计。
// ===========================================================================
class Comparator {
public:
    Comparator() = default;
    Comparator(const Comparator&) = delete;
    Comparator& operator=(const Comparator&) = delete;
    virtual ~Comparator() = default;

    // 三路比较：a < b 返回负，a == b 返回 0，a > b 返回正。
    virtual int Compare(const Slice& a, const Slice& b) const = 0;

    // 比较器的唯一标识，会落盘持久化
    virtual const char* Name() const = 0;

    // 下面两个方法用于生成"尽可能短的分隔键"，服务于 SSTable 的索引块。
    //
    // 场景：索引块里为每一个 data block 存一个"起始键"。这个键只用于二分定位，
    // 它本身不需要是真实存在的 key，只要满足  start <= 块内所有 key < limit。
    // 键越短，索引块越小，内存占用越少。
    //
    // FindShortestSeparator: 已知 *start < limit，把 *start 改成仍满足
    //                        *start < limit 的最短字符串。
    // FindShortSuccessor:    把 *key 改成严格大于原值的最短字符串。
    virtual void FindShortestSeparator(std::string* start,
                                       const Slice& limit) const = 0;
    virtual void FindShortSuccessor(std::string* key) const = 0;
};

// 字节序比较器。全局单例，返回的是静态对象，生命周期与进程相同。
const Comparator* BytewiseComparator();

}  // namespace tinystore
