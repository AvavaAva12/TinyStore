#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace tinystore {

// ===========================================================================
// Arena —— 一次性分配、整体释放的内存池（bump allocator）
// ===========================================================================
//
// 【解决什么问题】
// MemTable 里的每个节点都是一次小对象分配（几十到几百字节），写入 QPS 高时
// 每秒会产生几十万次 new。直接 malloc 有三个问题：
//
//   1. 分配慢。malloc 需要维护 free list、处理锁竞争（多线程下尤其明显）。
//      Arena 的快路径只是 `ptr += size; remaining -= size;` 两条指令。
//
//   2. 内存碎片。大量小对象反复分配释放，堆里会留下许多不连续的小空洞，
//      即使总空闲内存够，也可能分配不出一块较大的连续内存。
//
//   3. 释放成本高。MemTable 被 flush 后，里面的所有节点都要释放。逐个 delete
//      是 O(n) 次系统调用；Arena 只需 delete[] 几个大块，是 O(块数)。
//
// 【核心设计：只分配，不单独释放】
// Arena 只提供 Allocate，没有 Free。所有对象随 Arena 一起消亡。
// 这换来的是极致的分配速度，代价是灵活性 —— 它只适合"生命周期一致的一批对象"
// （MemTable 的所有节点正是如此：MemTable 被丢弃时，节点全部同时失效）。
//
// 【极其重要的约束：不会调用析构函数】
// Arena 释放内存时只做 delete[] char，不会逐个调用对象的析构函数。
// 因此：
//   * 放进 Arena 的对象必须是 trivially destructible（比如 SkipList 的
//     节点结构体，其内部只含 POD 和指针），或者其析构函数无副作用；
//   * 绝不能把 std::string、std::vector 这类持有堆资源的对象放进 Arena ——
//     它们的堆内存会永久泄漏，且 ASAN 会报 leak。
//   * 正确用法是 placement new 构造 + 手动管理：
//         void* mem = arena.AllocateAligned(sizeof(Node));
//         Node* node = new (mem) Node(key);   // 只构造，不分配
//     析构时什么都不做，靠 Arena 整体回收。
//
// 【ASAN 的局限性（进阶话题）】
// ASAN 无法检测 Arena 内部的越界：它只知道有几个大块被 new 出来，
// 块内被切成多少个小对象它并不知情。要检测需要用到 ASAN 的
// "container annotation" 接口（__sanitizer_annotate_contiguous_container），
// 手动把已分配/未分配区域的边界告知 ASAN。
// 本项目在 W8 的调试阶段会补上这一层。
// ===========================================================================
class Arena {
public:
    Arena();

    // Arena 不可拷贝、不可移动：它持有裸指针，且对象在内部有固定地址，
    // 移动会导致所有已发出的指针失效。
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    // 释放所有内存块。注意：不会调用任何对象的析构函数（见上方说明）。
    ~Arena();

    // 分配 bytes 字节，地址保证满足 alignof(std::max_align_t) 之外的无特殊要求。
    // 返回的内存内容是未初始化的。
    // 要求 bytes > 0。
    char* Allocate(size_t bytes);

    // 分配并满足最严格的基础对齐（通常是 16 字节）。
    // 用于 placement new 构造含指针成员的结构体。
    char* AllocateAligned(size_t bytes);

    // 已向系统申请的总字节数（包含管理开销的估算）。
    //
    // 用 atomic + memory_order_relaxed 的理由：
    // 这个值会被后台 flush 线程读取（判断 MemTable 是否该落盘），
    // 而写入方是前台写线程，存在跨线程访问，因此必须是 atomic 以避免 data race。
    // 但用 relaxed 就够了 —— 它只是一个**近似统计值**，不需要与任何其它
    // 内存操作建立同步关系：读到一个稍旧的值最多导致 flush 晚几毫秒触发，
    // 不影响正确性。若用 seq_cst 则会在每次写操作时插入不必要的内存屏障，
    // 白白损失性能。这是"按需求选择最弱内存序"的典型例子。
    size_t MemoryUsage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    char* AllocateFallback(size_t bytes);
    char* AllocateNewBlock(size_t block_bytes);

    // 当前块的剩余区域：[alloc_ptr_, alloc_ptr_ + alloc_bytes_remaining_)
    char* alloc_ptr_;
    size_t alloc_bytes_remaining_;

    // 所有已分配的块，析构时统一释放
    std::vector<char*> blocks_;

    std::atomic<size_t> memory_usage_;
};

}  // namespace tinystore
