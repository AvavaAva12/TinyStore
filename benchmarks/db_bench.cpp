#include <atomic>
#include <benchmark/benchmark.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "tinystore/db.h"

namespace tinystore {
namespace {

static std::string BenchDir() {
  std::string dir;
  Env::Default()->GetTestDirectory(&dir);
  return dir + "/bench-db";
}

static void Cleanup(const std::string& name) {
  std::error_code ec;
  std::filesystem::remove_all(name, ec);
}

// 单线程顺序写：每个 Put 都只能等自己那一次 fsync，没有可合并的对象。
// 这是 Group Commit 的"下界"——用来对比并发写被合并后的收益。
static void BM_WriteSequential(benchmark::State& state) {
  Options opt;
  opt.create_if_missing = true;
  const std::string name = BenchDir();
  Cleanup(name);
  DB* db;
  DB::Open(opt, name, &db);

  int i = 0;
  for (auto _ : state) {
    db->Put("key" + std::to_string(i), "value");
    ++i;
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));

  delete db;
  Cleanup(name);
}
BENCHMARK(BM_WriteSequential);

// 多线程并发写：同一时刻到达的多个 Put 会在 WAL 处被 Group Commit 合并成
// 一组，整组只做一次 fsync。线程越多，可合并的批次越大，单条写的平均
// fsync 开销越低 —— 通常带来明显的 QPS 提升。
static void BM_WriteConcurrent(benchmark::State& state) {
  const int nthreads = static_cast<int>(state.range(0));
  Options opt;
  opt.create_if_missing = true;
  const std::string name = BenchDir();
  Cleanup(name);
  DB* db;
  DB::Open(opt, name, &db);

  std::atomic<int> counter{0};
  for (auto _ : state) {
    std::vector<std::thread> ts;
    ts.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) {
      ts.emplace_back([&] {
        const int k = counter.fetch_add(1, std::memory_order_relaxed);
        db->Put("key" + std::to_string(k), "value");
      });
    }
    for (auto& t : ts) t.join();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * nthreads);

  delete db;
  Cleanup(name);
}
BENCHMARK(BM_WriteConcurrent)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// 并发读（无锁快照读）：验证读路径不取写锁，多个读者互不被阻塞。
static void BM_ReadConcurrent(benchmark::State& state) {
  const int nthreads = static_cast<int>(state.range(0));
  Options opt;
  opt.create_if_missing = true;
  const std::string name = BenchDir();
  Cleanup(name);
  DB* db;
  DB::Open(opt, name, &db);

  // 预填 10000 条，制造一个有内容的 MemTable
  for (int i = 0; i < 10000; ++i) {
    db->Put("key" + std::to_string(i), "value");
  }

  std::string got;
  for (auto _ : state) {
    std::vector<std::thread> ts;
    ts.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) {
      ts.emplace_back([&] {
        std::string g;
        db->Get("key" + std::to_string(t), &g);
      });
    }
    for (auto& t : ts) t.join();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * nthreads);

  delete db;
  Cleanup(name);
}
BENCHMARK(BM_ReadConcurrent)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

}  // namespace
}  // namespace tinystore

BENCHMARK_MAIN();
