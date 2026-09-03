#pragma once

#include <string>

#include "tinystore/comparator.h"
#include "tinystore/env.h"
#include "tinystore/status.h"

namespace tinystore {

class WriteBatch;  // 仅作参数类型，前向声明即可

// ===========================================================================
// Options —— 打开数据库时的配置
// ===========================================================================
struct Options {
  // 文件 / 线程 / 时钟抽象层。默认用进程单例 PosixEnv。
  Env* env = Env::Default();

  // key 的排序规则。默认字节序。**一旦写入就不能换**，否则读出的结果会错乱
  // （这也是 InternalKeyComparator::Name() 要落盘校验的原因，W4 用上）。
  const Comparator* comparator = BytewiseComparator();

  // 数据库目录不存在则自动创建
  bool create_if_missing = false;

  // 数据库目录已存在则报错（防止误打开别人的库，覆盖其数据）
  bool error_if_exists = false;
};

// ===========================================================================
// DB —— 数据库句柄（纯虚接口，具体实现见 db_impl.h 的 DBImpl）
// ===========================================================================
//
// 这是用户对外的唯一入口。W3 把它落地成"能写、能读、能持久化"的最小引擎：
//   * 写：WriteBatch 先追加进 WAL（一次 Group Commit 合并多个写、只 fsync 一次），
//        再回放到内存 MemTable；
//   * 读：直接查 MemTable，走无锁快照读（不被前台写阻塞）；
//   * 恢复：打开时重放 WAL 把 MemTable 重建出来。
// （落 SSTable / Compaction / 版本管理是 W4 及以后的事。）
class DB {
public:
  virtual ~DB() = default;

  // 打开（或创建）一个名为 name 的数据库，成功时 *dbptr 接管所有权。
  // 失败（目录问题、IO 错误、WAL 损坏等）时 *dbptr 置空并返回非 OK 状态。
  static Status Open(const Options& options, const std::string& name,
                     DB** dbptr);

  // 写入一个键值对（等价于 Write 一个只含一条 Put 的 WriteBatch）
  virtual Status Put(const Slice& key, const Slice& value) = 0;

  // 删除一个键（写一条墓碑；真正的空间回收留给 W4 的 Compaction）
  virtual Status Delete(const Slice& key) = 0;

  // 快照读：返回 key 在"当前已提交的最大 sequence"下的值。
  //   OK + *value          —— 找到
  //   NotFound             —— 不存在，或最新可见版本是删除墓碑
  virtual Status Get(const Slice& key, std::string* value) = 0;

  // 原子地写入一批修改。W3 的 Group Commit 在此实现：
  // 多个并发 Write 会被合并成一组，整组只做一次 WAL fsync。
  virtual Status Write(const WriteBatch& batch) = 0;
};

}  // namespace tinystore
