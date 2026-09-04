#pragma once

#include <string>
#include <vector>

#include "tinystore/slice.h"

namespace tinystore {

// ===========================================================================
// FilterPolicy —— 布隆过滤器接口的抽象
// ===========================================================================
//
// 【为什么需要它】
// Get 在 SSTable 层是"按 key 二分定位数据块 + 读块"的过程。如果 key 根本不在这个
// 文件里，这两次 IO 就完全浪费了。布隆过滤器用极少的内存（每 key 约 10 bit）
// 给出"这个 key 一定不在 / 可能在"的判断：说"不在"就直接跳过整个文件，
// 把随机点查的 IO 从"每文件一次"降到"真正可能包含时一次"。
//
// 这是一个经典的"用少量内存换 IO"的取舍，也是 LSM 读放大问题最核心的解药之一。
class FilterPolicy {
public:
  virtual ~FilterPolicy() = default;

  // 过滤器实现的唯一标识，落盘到 SSTable 的 metaindex，打开时用它匹配策略，
  // 不匹配直接拒绝（与 Comparator::Name 同样的"防静默错误"思想）。
  virtual const char* Name() const = 0;

  // 把 n 个 key 压缩成一个过滤器位图，写入 *dst。
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const = 0;

  // key 是否可能存在于 filter 所描述的 key 集合里。
  // 返回 false 表示"一定不在"（调用方可安全跳过整个 SSTable）；
  // 返回 true 表示"可能在"（有极小概率误报）。
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// 工厂：返回一个 bits_per_key 位/key 的布隆过滤器策略（标准 LevelDB 参数 ≈ 10）。
// 返回的指针由**调用方拥有**，不再使用时需 delete。
const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

// 进程级默认策略（10 bit/key 的布隆过滤器），具有静态存储期、随进程退出销毁。
// 借用者（如 Options）只拿指针、不拥有，因此 Options 的拷贝与析构都不会泄漏，
// 用户也无需手动释放——这正是它存在的理由：Options 是可被值拷贝的轻量配置对象，
// 若在默认成员初始化里 new，每一次 `Options opt;` 都会漏一个策略对象。
const FilterPolicy* DefaultFilterPolicy();

}  // namespace tinystore
