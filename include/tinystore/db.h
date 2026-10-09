#pragma once

#include <memory>
#include <string>

#include "tinystore/comparator.h"
#include "tinystore/env.h"
#include "tinystore/filter_policy.h"
#include "tinystore/internal_key.h"
#include "tinystore/iterator.h"
#include "tinystore/status.h"

namespace tinystore {

class WriteBatch;  // 仅作参数类型，前向声明即可

// ---------------------------------------------------------------------------
// ReadOptions —— 读操作的参数
// ---------------------------------------------------------------------------
struct ReadOptions {
  // 只返回 sequence <= snapshot 的版本。
  //
  // 默认 kMaxSequenceNumber，即"读当前所有已提交数据"，与 Get 的行为一致。
  // 传一个具体值即可做历史读（time travel）：拿到某个时刻的数据库视图。
  SequenceNumber snapshot = kMaxSequenceNumber;
};

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

  // --- W4：持久化层调参 ---

  // MemTable 达到此大小（字节）就触发 flush 成 SSTable。默认 4MB。
  // 越大：flush 次数少、写放大低，但崩溃后要重放的 WAL 更长、内存占用更高。
  size_t write_buffer_size = 4 * 1024 * 1024;

  // SSTable 单个数据块的目标大小（约值）。默认 4KB：块越大随机读 IO 更贵、
  // 但索引更小；块越小定位越精细、过滤收益更高。
  size_t block_size = 4096;

  // 布隆过滤器策略（每 key 约 10 bit 时误报率 ~1%）。默认开启内置布隆。
  // 默认是进程级单例（借用、不拥有），所以 Options 可以随意值拷贝而不泄漏；
  // 想要自定义参数时，用 NewBloomFilterPolicy 自建并自行 delete。
  // 设置 nullptr 可关闭，以便对比"无过滤"时的读放大。
  const FilterPolicy* filter_policy = DefaultFilterPolicy();
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

  // 创建一个有序迭代器，范围扫描整个数据库（MemTable + 全部 SSTable）。
  //
  // 【返回的迭代器看到什么】
  //   * 按 user_key 字典序升序，每个 user_key 只出现一次；
  //   * 取该 snapshot 下可见的最新版本；
  //   * **删除墓碑不出现**（被删掉的 key 就当它不存在，与 Get 返回 NotFound 一致）。
  //
  // 【为什么用 unique_ptr 而不是裸指针】
  // 迭代器持有 MemTable 与 Version 的引用，忘记释放就会让这些对象永远无法回收。
  // 用智能指针把这个坑变成编译期错误。
  //
  // 【快照一致性】
  // options.snapshot 固定后，多次 Next 之间即使有并发写入，本次扫描的视图也不会
  // 变化——这正是 MVCC 的价值。默认取 kMaxSequenceNumber 表示"读当前最新"。
  //
  // 【生命周期】返回的迭代器必须在 DB 关闭前销毁（或先 delete DB 再销毁迭代器）。
  virtual std::unique_ptr<Iterator> NewIterator(
      const ReadOptions& options) const = 0;
};

}  // namespace tinystore
