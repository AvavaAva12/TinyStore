#include "tinystore/arena.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace tinystore {

namespace {

// ---------------------------------------------------------------------------
// 块大小：4KB
//
// 选 4KB 不是随意的，它对应：
//   * x86-64 上一个内存页的大小（mmap 的粒度）；
//   * 大多数 malloc 实现中 fastbin / tcache 覆盖的尺寸上限。
// 太大 -> 单次分配的尾部浪费多；太小 -> 向系统申请的次数变多。
// 4KB 是实践中比较均衡的选择，后续可以用 benchmark 验证。
// ---------------------------------------------------------------------------
constexpr size_t kBlockSize = 4096;

// ---------------------------------------------------------------------------
// 对齐：alignof(std::max_align_t)
//
// malloc 返回的地址保证满足任何基础类型的对齐要求，即
// alignof(std::max_align_t)（x86-64 上通常是 16，因为 long double / SSE 类型
// 需要 16 字节对齐）。Arena 要作为 malloc 的替代品，就必须提供同等保证。
// ---------------------------------------------------------------------------
constexpr size_t kAlignment = alignof(std::max_align_t);

static_assert((kAlignment & (kAlignment - 1)) == 0,
              "kAlignment must be a power of two for the masking trick below");

}  // namespace

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {
    // 构造函数里刻意不预分配第一个块：Arena 可能创建后完全不使用
    // （例如一个空 MemTable），惰性分配可以避免无谓的内存占用。
    // 首次 Allocate 时会走 AllocateFallback 补上。
}

Arena::~Arena() {
    // 只释放块内存，不调用任何对象析构函数 —— 见头文件的详细说明。
    for (char* block : blocks_) {
        delete[] block;
    }
    blocks_.clear();
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
    auto* result = new char[block_bytes];
    blocks_.push_back(result);

    // + sizeof(char*) 是估算 blocks_ 这个 vector 自身为每个元素付出的内存。
    // 这是一处工程上的近似：真实的 vector 容量可能大于元素个数（指数扩容），
    // 精确统计反而得不偿失。这里要表达的是"内存占用量级"而非精确值。
    memory_usage_.fetch_add(block_bytes + sizeof(char*),
                            std::memory_order_relaxed);
    return result;
}

char* Arena::AllocateFallback(size_t bytes) {
    // ---------------------------------------------------------------------
    // 大对象特殊处理：超过块大小 1/4 的请求，单独开一块独占
    //
    // 为什么不直接塞进当前块？
    // 假设当前块只剩 100 字节，而请求 2000 字节。若开一个新 4KB 块来放它，
    // 那当前块剩下的 100 字节就会被跳过浪费（因为 alloc_ptr_ 会移到新块）。
    // 给大对象单独开一块刚好够用的内存，既避免了内部碎片，又保留了当前块
    // 的剩余空间给后续的小对象。
    //
    // 注意：这条路径返回的内存**不会**被登记到 alloc_ptr_ / remaining，
    // 因为它是一次性的，不参与后续的 bump 分配。
    // ---------------------------------------------------------------------
    if (bytes > kBlockSize / 4) {
        return AllocateNewBlock(bytes);
    }

    // 开一个新块作为当前块，然后走正常的快路径逻辑
    alloc_ptr_ = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;

    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
}

char* Arena::Allocate(size_t bytes) {
    // 不允许分配 0 字节：返回同一个指针会让"两个不同对象地址相同"这种
    // 隐蔽 bug 有机可乘（比如跳表节点判等）。
    assert(bytes > 0);

    // 快路径：当前块剩余空间足够，直接移动指针。
    // 这两行就是 Arena 的全部性能优势所在 —— 没有锁、没有系统调用、
    // 没有 free list 查找，通常可以被编译器完全内联。
    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    return AllocateFallback(bytes);
}

char* Arena::AllocateAligned(size_t bytes) {
    assert(bytes > 0);

    // 位掩码取模：kAlignment 是 2 的幂，所以 `addr & (kAlignment - 1)`
    // 等价于 `addr % kAlignment`，但只需一条 and 指令。
    const auto current_mod =
        reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlignment - 1);

    // 需要跳过的字节数，让结果落到下一个对齐边界上
    const size_t slop = (current_mod == 0) ? 0 : (kAlignment - current_mod);
    const size_t needed = bytes + slop;

    char* result = nullptr;
    if (needed <= alloc_bytes_remaining_) {
        result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
    } else {
        // 慢路径。这里可以直接用 AllocateFallback 的返回值，因为：
        //   * 大对象分支走的是 `new char[]`，天然满足 kAlignment 对齐；
        //   * 新块分支同样来自 `new char[]`，块首地址也是对齐的。
        // 换句话说，`operator new` 的返回值已经至少是
        // __STDCPP_DEFAULT_NEW_ALIGNMENT__ 对齐的，无需二次校正。
        result = AllocateFallback(bytes);
    }

    assert((reinterpret_cast<uintptr_t>(result) & (kAlignment - 1)) == 0);
    return result;
}

}  // namespace tinystore
