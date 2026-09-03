#include "tinystore/arena.h"

#include <cstdlib>
#include <vector>

#include <benchmark/benchmark.h>

using tinystore::Arena;

namespace {

constexpr int kAllocationsPerIteration = 10000;

// ---------------------------------------------------------------------------
// Arena 分配：只分配，不单独释放，最后整体回收
// ---------------------------------------------------------------------------
void BM_ArenaAllocate(benchmark::State& state) {
    const size_t alloc_size = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        (void)_;
        Arena arena;
        for (int i = 0; i < kAllocationsPerIteration; ++i) {
            char* p = arena.Allocate(alloc_size);
            // 阻止编译器把整个循环当作无用代码优化掉
            benchmark::DoNotOptimize(p);
        }
        benchmark::ClobberMemory();
    }
}

// ---------------------------------------------------------------------------
// malloc/free 对照组
// ---------------------------------------------------------------------------
void BM_MallocFree(benchmark::State& state) {
    const size_t alloc_size = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        (void)_;
        std::vector<char*> ptrs;
        ptrs.reserve(kAllocationsPerIteration);

        for (int i = 0; i < kAllocationsPerIteration; ++i) {
            ptrs.push_back(static_cast<char*>(std::malloc(alloc_size)));
        }
        benchmark::DoNotOptimize(ptrs.data());

        for (char* p : ptrs) {
            std::free(p);
        }
        benchmark::ClobberMemory();
    }
}

// ---------------------------------------------------------------------------
// new/delete 对照组
// ---------------------------------------------------------------------------
void BM_NewDelete(benchmark::State& state) {
    const size_t alloc_size = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        (void)_;
        std::vector<char*> ptrs;
        ptrs.reserve(kAllocationsPerIteration);

        for (int i = 0; i < kAllocationsPerIteration; ++i) {
            ptrs.push_back(new char[alloc_size]);
        }
        benchmark::DoNotOptimize(ptrs.data());

        for (char* p : ptrs) {
            delete[] p;
        }
        benchmark::ClobberMemory();
    }
}

}  // namespace

BENCHMARK(BM_ArenaAllocate)->Arg(32)->Arg(128)->Arg(1024);
BENCHMARK(BM_MallocFree)->Arg(32)->Arg(128)->Arg(1024);
BENCHMARK(BM_NewDelete)->Arg(32)->Arg(128)->Arg(1024);

BENCHMARK_MAIN();
