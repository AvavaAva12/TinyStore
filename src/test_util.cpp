#include "tinystore/test_util.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>

namespace tinystore {
namespace {

// 当前注入的崩溃点，kCrashPointNone 表示不崩。
//
// 用 relaxed 而非更强的序：这里没有要保护的发布关系——测试进程要么在
// SetCrashPointForTesting 之后 fork（子进程通过 exec/fork 继承初值），
// 要么完全不设置。atomic 只是为了避免"编译器把这个变量当常量优化掉"
// 以及让 TSAN 满意（若它被当作跨线程共享的普通变量）。
std::atomic<int> g_crash_point{kCrashPointNone};

}  // namespace

void SetCrashPointForTesting(int point) {
  g_crash_point.store(point, std::memory_order_relaxed);
}

void MaybeCrashForTesting(int point) {
  if (g_crash_point.load(std::memory_order_relaxed) != point) return;
  // _exit 而非 abort：不跑 atexit、不析构静态对象、不刷缓冲——与 kill -9 一致。
  // 用 _exit(0) 会让父进程误以为"子进程成功退出"，因此必须用非零码。
  ::_exit(97);
}

}  // namespace tinystore