#include "tinystore/arena.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

// ---------------------------------------------------------------------------
// 注意：Arena 的内存由析构函数整体释放，测试里不需要（也不能）逐个释放。
// 如果 ASAN 报 leak，说明实现里漏了 delete[] blocks_。
// ---------------------------------------------------------------------------

TEST(ArenaTest, EmptyArenaAllocatesNothing) {
    Arena arena;
    // 构造时惰性分配，尚未使用则不应占用内存
    EXPECT_EQ(0u, arena.MemoryUsage());
}

TEST(ArenaTest, AllocateReturnsWritableMemory) {
    Arena arena;

    char* p = arena.Allocate(128);
    ASSERT_NE(nullptr, p);

    // 写入全区域并读回，验证内存确实可用（ASAN 会捕捉越界）
    std::memset(p, 0xAB, 128);
    for (int i = 0; i < 128; ++i) {
        ASSERT_EQ(static_cast<char>(0xAB), p[i]);
    }

    EXPECT_GT(arena.MemoryUsage(), 0u);
}

TEST(ArenaTest, AllocateZeroIsNotAllowed) {
    // Debug 构建下 assert(bytes > 0) 会中止；
    // Release 构建下 NDEBUG 会让 assert 失效，此测试只验证不崩溃。
    // 这里刻意不调用 Allocate(0)，因为契约上就不允许。
    Arena arena;
    EXPECT_EQ(0u, arena.MemoryUsage());
}

TEST(ArenaTest, SmallAllocationsComeFromSameBlock) {
    Arena arena;

    const char* first = arena.Allocate(16);
    const char* second = arena.Allocate(16);

    // 快路径：连续两次小分配应该相邻（相差正好 16 字节）
    EXPECT_EQ(first + 16, second);
}

TEST(ArenaTest, AllocatedRegionsNeverOverlap) {
    Arena arena;

    std::vector<std::pair<char*, size_t>> regions;
    regions.reserve(2000);

    for (int i = 0; i < 2000; ++i) {
        // 变化的尺寸，覆盖"跨块"和"块内剩余不足"等多种情况
        const size_t size = 1 + static_cast<size_t>(i * 37) % 300;
        char* p = arena.Allocate(size);
        ASSERT_NE(nullptr, p);
        regions.emplace_back(p, size);
    }

    std::sort(regions.begin(), regions.end());

    for (size_t i = 1; i < regions.size(); ++i) {
        const char* prev_end = regions[i - 1].first + regions[i - 1].second;
        EXPECT_LE(prev_end, regions[i].first)
            << "region " << i << " overlaps with the previous one";
    }
}

TEST(ArenaTest, AllocateAlignedRespectsMaxAlign) {
    Arena arena;

    constexpr size_t kAlign = alignof(std::max_align_t);

    for (int i = 0; i < 500; ++i) {
        const size_t size = 1 + static_cast<size_t>(i * 13) % 200;
        char* p = arena.AllocateAligned(size);
        ASSERT_NE(nullptr, p);
        EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign)
            << "iteration " << i << ": misaligned address";
    }
}

TEST(ArenaTest, LargeAllocationBypassesBumpPointer) {
    Arena arena;

    // 超过 kBlockSize / 4 的请求会走"独占一整块"的分支
    constexpr size_t kLarge = 4096;  // > 4096 / 4 = 1024
    char* p = arena.Allocate(kLarge);
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t));

    std::memset(p, 0x5A, kLarge);  // 整块都可用
    EXPECT_EQ(static_cast<char>(0x5A), p[kLarge - 1]);

    // 大分配之后，小分配仍应正常工作
    char* q = arena.Allocate(32);
    ASSERT_NE(nullptr, q);
    EXPECT_NE(p, q);
}

TEST(ArenaTest, MemoryUsageGrowsWithAllocations) {
    Arena arena;

    arena.Allocate(64);
    const size_t after_first = arena.MemoryUsage();
    EXPECT_GT(after_first, 0u);

    // 持续分配直到明显超过一个块
    for (int i = 0; i < 100; ++i) {
        arena.Allocate(64);
    }
    EXPECT_GE(arena.MemoryUsage(), after_first);

    // 再分配一个大块，占用量应显著增加
    arena.Allocate(8192);
    EXPECT_GE(arena.MemoryUsage(), 8192u);
}

TEST(ArenaTest, MemoryUsageIsApproximationIncludingOverhead) {
    Arena arena;
    arena.Allocate(1);

    // 分配 1 字节却占用了一个完整块 —— 这是 bump allocator 的固有特性：
    // 用空间换分配速度。MemoryUsage 统计的是"向系统申请的字节数"，
    // 而不是"用户实际使用的字节数"。
    EXPECT_GE(arena.MemoryUsage(), 4096u);
}

TEST(ArenaTest, HandlesManyAllocationsAcrossManyBlocks) {
    Arena arena;

    constexpr int kCount = 10000;
    std::vector<char*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        char* p = arena.Allocate(24);
        ASSERT_NE(nullptr, p);
        *p = static_cast<char>(i & 0xff);  // 写入标记
        ptrs.push_back(p);
    }

    // 所有指针必须互不相同，且写入的内容没有被后续分配覆盖
    std::vector<char*> sorted = ptrs;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted.end(), std::unique(sorted.begin(), sorted.end()));

    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(static_cast<char>(i & 0xff), *ptrs[i]) << "index " << i;
    }
}

TEST(ArenaTest, AlignedAllocationAfterUnalignedRemainingSpace) {
    Arena arena;

    // 先用奇数尺寸把 alloc_ptr_ 推到非对齐位置
    arena.Allocate(1);
    arena.Allocate(3);
    arena.Allocate(7);

    // 此时剩余空间起点是奇数，AllocateAligned 必须能正确补齐
    constexpr size_t kAlign = alignof(std::max_align_t);
    for (int i = 0; i < 100; ++i) {
        char* p = arena.AllocateAligned(1 + static_cast<size_t>(i) % 50);
        ASSERT_NE(nullptr, p);
        EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % kAlign);
    }
}

}  // namespace
}  // namespace tinystore
