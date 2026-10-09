#pragma once

// ===========================================================================
// 崩溃注入点 —— 仅供测试使用
// ===========================================================================
//
// 【为什么需要它】
// 崩溃恢复的正确性依赖**操作的先后顺序**，而这些顺序在正常运行时是不可见的：
//
//   flush 的原子性来自"SSTable 先落盘 → 再 LogAndApply 登记 → 才删旧 WAL"。
//   这个顺序如果错了，表现是"重启后数据丢失"，但在所有顺利的测试里都看不出问题。
//
//   普通的"跑一遍再重开"只能验证顺序**恰好**正确时的结果，无法验证：
//   若在中间任何一步崩溃，恢复逻辑能不能兜住。
//
// 【机制】
// 业务代码在若干关键点调用 MaybeCrashForTesting(point)。它做一次 relaxed 原子读，
// 与全局变量相等就立刻 _exit 模拟"进程被 kill -9"。
//
// 生产路径上 SetCrashPointForTesting 从不被调用，该变量恒为 kCrashPointNone，
// 于是每次调用只是一次原子读 + 比较，开销可忽略。这是 LevelDB SyncPoint 的
// 同款做法：把"崩溃"变成可被测试精确触发的输入，而不是靠运气等它发生。
//
// 【为什么用 _exit 而不是 abort】
// _exit 不跑 atexit、不析构静态对象、不 flush 任何缓冲——这正是 kill -9 的语义。
// abort 会先执行 atexit 处理器，可能把该保留的东西"帮忙"写回去，掩盖真实缺陷。
//
// ===========================================================================

namespace tinystore {

// 崩溃注入点。取值必须与 SetCrashPointForTesting 传入的一致。
enum CrashPoint : int {
  kCrashPointNone = 0,

  // WAL 已 AddRecord + Sync 成功，但还没 InsertInto MemTable。
  // 此刻写入是"已对用户承诺成功"的（fsync 已过），但内存视图里没有。
  // 恢复后必须从 WAL 重放出这条记录。
  kCrashPointAfterWalSync,

  // flush 的 SSTable 已完整落盘（Finish + Sync + Close 都成功），
  // 但尚未 LogAndApply 登记进 MANIFEST。
  // 恢复后这个 SSTable 是孤儿，必须被清理，且数据必须仍能从旧 WAL 重放出来。
  kCrashPointAfterFlushSstWrite,

  // MANIFEST 已提交（新 SSTable 已登记、log_number 已切换），
  // 但旧 WAL 尚未删除、新 WAL 尚未建好。
  kCrashPointAfterFlushCommit,

  // compaction 的输出文件已全部落盘，但尚未 LogAndApply 提交。
  // 恢复后输入文件必须仍然可用（不能因为输出文件已存在就当作已提交）。
  kCrashPointAfterCompactionOutput,
};

// 设置下一个崩溃点。仅供测试调用。
void SetCrashPointForTesting(int point);

// 若当前注入点等于 point，立刻终止进程模拟突然死亡。
// 业务代码在关键位置调用它；正常情况下是个廉价的原子读。
void MaybeCrashForTesting(int point);

}  // namespace tinystore