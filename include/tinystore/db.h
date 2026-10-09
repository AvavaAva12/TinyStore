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

// ===========================================================================
// Snapshot —— 一个稳定的读视图（时间旅行的句柄）
// ===========================================================================
//
// 【为什么不能只把 sequence 抄进 ReadOptions】
// ReadOptions::snapshot 是一个裸数字，它能表达"读 seq<=100 的版本"，但表达不了
// "从现在起，我要一致地读 seq<=100 这个视图，直到我主动放弃"。缺的是**生命周期**：
// 快照需要被数据库知道"还有人在读它"，这样压缩才能保留它依赖的旧版本。
//
// 举一个会真实出错的例子：
//   1. 读到一半，取 ReadOptions{snapshot=100}
//   2. 期间别人写入 seq=200，并触发一次 compaction
//   3. compaction 压平了 seq=100 与 seq=200，旧版本被丢弃
//   4. 回到第 1 步的扫描 —— 若序列号还在，却读不到 seq<=100 的版本了
// 有了 Snapshot 句柄，第 3 步就知道"100 还有活跃读者"，压缩必须保留它。
//
// 【典型用法】
//     const Snapshot* snap = db->GetSnapshot();
//     ReadOptions ro;
//     ro.snapshot = snap->sequence();
//     ... 多次 Get / 迭代，全程一致 ...
//     db->ReleaseSnapshot(snap);
// 快照必须先于 DB 释放（与 Iterator 同样的约定）。
class DBImpl;  // 仅用于 friend 声明：快照只能由数据库实现创建

class Snapshot {
public:
  // 该快照对应的 sequence：读它只能看到 seq <= 这个值的版本。
  SequenceNumber sequence() const { return sequence_; }

  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;

private:
  // 只有 DBImpl 能创建快照。把它设为 private 是为了让"快照点从哪来"这件事
  // 只有一处决定（GetSnapshot 里读 last_sequence_），避免调用方凭空造一个
  // sequence 出来——那种快照数据库根本不知道，旧版本可能早被 compaction 丢弃。
  friend class DBImpl;
  explicit Snapshot(SequenceNumber seq) : sequence_(seq) {}
  ~Snapshot() = default;

  SequenceNumber sequence_;
};

// ---------------------------------------------------------------------------
// ReadOptions —— 读操作的参数
// ---------------------------------------------------------------------------
struct ReadOptions {
  // 只返回 sequence <= snapshot 的版本。
  //
  // 默认 kMaxSequenceNumber，即"读当前所有已提交数据"，与 Get 的行为一致。
  // 传一个具体值即可做历史读（time travel）：拿到某个时刻的数据库视图。
  SequenceNumber snapshot = kMaxSequenceNumber;

  // 从 Snapshot 句柄直接构造，省去手抄 sequence() 的样板：
  //     ReadOptions ro(db->GetSnapshot());
  explicit ReadOptions(const Snapshot* s)
      : snapshot(s != nullptr ? s->sequence() : kMaxSequenceNumber) {}
  ReadOptions() = default;
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

  // --- Compaction（LSM 的核心收益来源）---

  // L0 的文件数达到此值就触发 L0 -> L1。
  // L0 文件全部来自 flush，彼此可能键范围重叠，所以点查最坏要扫遍 L0 全部文件；
  // 限制它的数量就是给读放大设一个上界。
  size_t l0_compaction_trigger = 4;

  // level >= 1 各层的容量上限基准值。level i 的实际上限是
  //   max_level_bytes * max_level_bytes_multiplier^(i-1)
  // 逐层递增是 Leveled Compaction 的核心思想：让绝大多数数据沉在底层，
  // 顶层只保留少量"刚写入、可能马上被合并掉"的数据。
  uint64_t max_level_bytes = 8u << 20;  // 8MB
  uint64_t max_level_bytes_multiplier = 10;

  // 最多允许有多少层（含 L0）。层数受 level 编号位数限制，超出会写爆内部键。
  int max_num_levels = 7;

  // 单个 compaction 输出文件的目标大小。归并结果超过它就切成多个文件——
  // 否则一次大归并可能产出一个巨大文件，下次读它的代价过高。
  uint64_t max_compaction_file_size = 2u << 20;  // 2MB

    // --- W8：SSTable 缓存（TableCache）容量上限 ---

    // 已打开的 SSTable 会缓存各自的索引块与过滤器，避免每次点查都重读 footer。
    // 但缓存若只增不减，长期运行会把**整个数据库**的索引常驻内存，内存占用无上限。
    // 超过此字节数时按 LRU 淘汰最久未使用的条目。
    //
    // 设为 0 表示不淘汰（等价于关闭缓存容量限制，行为与 W4~W7 一致）。
    //
    // 【为什么不按"打开文件数"限制】
    // 内存占用与文件**大小**强相关（索引块随数据量增长），而与文件个数关系较弱；
    // 按字节数限流才能真正约束内存。
    uint64_t max_table_cache_bytes = 64u << 20;  // 64MB

    // --- W8：Compaction 限流 ---

    // Compaction 写入的字节数上限 = 前台写入字节数 × 该比例。
    //
    // 【为什么需要限流】
    // Compaction 与前台写共享同一条磁盘带宽。若压缩长期跑得比写入快很多，
    // 它会持续抢占 IO、反复搬运同一批数据（"写放大"失控），把前台写入的
    // 延迟顶高。有界化后，压缩只能在"写入停下来"的那部分带宽里做功。
    //
    // 【逃生阀：积压上限】
    // 限流不能无限期地推迟压缩——否则 L0 会无限堆积文件，读放大爆掉。
    // 因此 L0 文件数一旦超过 l0_compaction_trigger × 该系数，就**无视限流强制压缩**。
    // 正常情况按限流节流，异常积压时兜底，两者结合才不会两头出问题。
    uint64_t compaction_max_write_amplification = 1;  // 压缩写入 <= 写入量 × 1

    // 强制压缩的积压倍数（相对 l0_compaction_trigger）。设为 0 表示不做兜底，
    // 此时若写入量长期为 0（纯读库），压缩会被限流无限期推迟。
    int compaction_backlog_factor = 4;
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

  // 带读选项的点查。options.snapshot 指定快照点，实现历史读。
  //
  // 【为什么必须有这个重载】
  // 没有它，Snapshot 只能用于 NewIterator——点查仍然永远读"最新"，
  // 快照对最常见的访问路径无效，上面的 Snapshot 注释里那个"扫描中途被压缩
  // 掉旧版本"的场景在点查上同样会发生。
  virtual Status Get(const Slice& key, std::string* value,
                     const ReadOptions& options) = 0;

  // ---- 快照句柄（见 Snapshot 的注释：为什么需要生命周期）----

  // 取一个读视图。返回值归调用方所有，必须配对 ReleaseSnapshot。
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

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
  // 【生命周期 —— 这是一条硬约束，违反会 use-after-free】
  // 返回的迭代器**必须先于 DB 销毁**。迭代器向 VersionSet 借用了 Table 引用，
  // 析构时负责归还；若 DB 已被 delete，VersionSet 随之释放，归还引用就会访问
  // 已释放对象。正确写法是用独立作用域把迭代器限制在 DB 存活期内：
  //     { std::unique_ptr<Iterator> it(db->NewIterator()); /* 用它 */ }
  //     delete db;
  // 这与 LevelDB 的约定一致。
  virtual std::unique_ptr<Iterator> NewIterator(
      const ReadOptions& options) const = 0;

  // ---- 统计与自省（W6：让 Compaction 的行为可被断言）----
  //
  // Compaction 是否真的生效，没法只靠"读出来的数据对不对"来判断——数据全对
  // 但文件数爆炸，同样是坏设计（读放大会随写入量线性上升，点查迟早超时）。
  // 下面这几个计数让测试能直接断言"压缩确实发生、文件数确实收敛"。

  // 各层的 SSTable 文件数，索引 0 即 L0；长度至少为 1。
  virtual std::vector<size_t> GetLevelFileCounts() const = 0;

  // 各层的总字节数，索引 0 即 L0。
  virtual std::vector<size_t> GetLevelBytes() const = 0;

  // SSTable 文件总数。
  virtual size_t NumTableFiles() const = 0;

  // 已打开的 SSTable 缓存条目数 / 占用字节数。用于断言"缓存确实有界"
  // （Options::max_table_cache_bytes 生效），否则内存占用会随写入无限增长。
  virtual size_t TableCacheEntries() const = 0;
  virtual uint64_t TableCacheBytes() const = 0;
};

}  // namespace tinystore
