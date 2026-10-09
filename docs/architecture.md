# TinyStore 架构说明

> 本文是**全局视角**的架构文档：数据怎么流动、文件怎么布局、机制为什么这样设计。
> 单个模块内部的取舍见代码注释；W1~W3 的设计决策见 [`design-notes.md`](design-notes.md)（该文写于 W3 阶段，W4~W9 的决策记录在本文第五章）。
> 当前已知的缺陷与待办见 [`code-review.md`](code-review.md)。

---

## 一、设计目标

一个**可运行、可验证**的 LSM 存储引擎，用于学习存储系统。三个硬性要求贯穿全部设计：

1. **绝不静默丢数据**。任何"损坏""失败"都必须显式上报，绝不降级为"不存在"或"正常结束"。这条原则在代码里反复出现，也是审查的一级判据。
2. **崩溃可恢复**。已返回 OK 的写入，在任意时刻被 `kill -9` 后重启都必须还在。
3. **读不被写阻塞**。除必要的引用计数外，读路径不加锁、不做 IO。

---

## 二、三条主路径

### 2.1 写路径

```
Put/Delete
  └─ WriteBatch 组装
      └─ DBImpl::Write  ── 加 mutex_，成为 leader
          ├─ 合并所有等待者成一个大 batch（Group Commit）
          ├─ 分配 sequence，写 WAL：log_->AddRecord + logfile_->Sync()
          ├─ WriteBatchInternal::InsertInto → MemTable（跳表）
          ├─ release 发布 last_sequence_
          ├─ 若 MemTable 超阈值 → CompactMemTable（同步，仍在 mutex_ 内）
          └─ 唤醒同组所有等待者
```

**关键决策**：

- **Group Commit**：多个并发写请求合并成一次 WAL 写入与一次 `fsync`。leader 干活，其余在条件变量上等。实测收益见 `design-notes.md` §7.6。
- **先写 WAL 再改内存**：WAL `fsync` 成功后才插入 MemTable。因此崩溃时不会出现"内存里有、WAL 没有"的不一致。
- **快照点截断**：`Get`/`NewIterator` 的读快照一律取 `min(用户请求值, last_sequence_)`。不能直接用默认值 `kMaxSequenceNumber`——写路径是"先插 MemTable、再 release 发布 `last_sequence_`"，这两步之间存在窗口，直接用会读到**尚未确认提交**的数据。

### 2.2 读路径

```
Get / NewIterator
  ├─ MemTable（跳表点查，RCU 引用）
  ├─ Version = VersionSet::current()（锁内取指针 + Ref）
  └─ 逐个 SSTable：从新到旧
      ├─ Bloom 过滤器整文件短路（"一定不含"直接跳过）
      ├─ index 块二分定位 data 块
      └─ data 块内二分 + 线性，MVCC 可见性判断
```

`NewIterator` 走 `DBIterator`：把 MemTable 与全部 SSTable 作为子迭代器**归并**，同 `user_key` 的多版本按 `internal_key` 顺序集中出现，逐版本做可见性判断后只吐出一个。

**MVCC 的核心不变式**：`internal_key = user_key + seq(7B) + type(1B)`，同 `user_key` 按 `seq` **降序**排列（最新在前）。因此：

- 定位"快照点可见的最新版本"= 找第一个 `seq <= snapshot` 的条目；
- 墓碑（`kTypeDeletion`）一旦命中就跳过该 `user_key` 的**全部**版本——否则等于"删了又复活"。

⚠️ **反向遍历不适用这条规则**：反向走时同 `user_key` 的版本按 `seq` **升序**出现（先老后新），照搬正向的"逐条过滤"会读到最老版本。`DBIterator::FindPrevVisibleEntry` 改为"整键扫过后取 `seq <= snapshot` 的最大值"。

### 2.3 Compaction

```
MaybeScheduleCompaction（写路径只"投任务"，不做 IO）
  └─ Env::Schedule → 后台线程
      └─ BackgroundCompactionTask
          └─ 循环直到收敛（guard 64 次）
              ├─ PickCompactionLevel：按"超限倍数"最大的层
              ├─ CompactionThrottled：写放大限流（积压时豁免）
              └─ CompactLevel
                  ├─ 选源文件（L0 整体 / 更高层挑一个）
                  ├─ 选下一层重叠文件（否则旧版本会复活）
                  ├─ DBIterator（compaction 模式）归并
                  ├─ 切分输出文件（max_compaction_file_size）
                  └─ LogAndApply 原子提交 + 删旧文件
```

**关键决策**：

- **原子提交**：新增输出文件与删除源文件写在**同一条** `VersionEdit` 里，一次 `LogAndApply` 落盘。绝不能分两次——中间崩溃会让源文件和输出文件都"存在但未登记"，或都不存在。
- **compaction 模式**：保留全部 `seq > snapshot` 的版本 + 快照点可见的最新版，丢弃更老的。`snapshot` 取**最早活跃快照**（无活跃快照时是 `kMaxSequenceNumber`，即只留最新版）。W6 之前这里硬编码 `kMaxSequenceNumber`，会把活跃快照依赖的版本一起压掉。
- **墓碑下沉**：非最底层的压缩**保留墓碑**，只有沉到最底层才丢弃。否则该 key 在更深层的旧版本会失去遮挡而"复活"。
- **按压力选层**：比较各层"实际值 / 阈值"，取最大者。固定顺序（先 L0 再自下而上）会让靠后的层长期得不到压缩——每轮都在处理更紧急的 L0，底层无限增长。
- **限流 + 逃生阀**：按写放大限流（compaction 输出 ≤ 写入量 × 系数）；但 L0 超过 `l0_trigger × backlog_factor` 时无视限流强制压缩。**只有限流没有逃生阀，纯读负载下压缩会被永久推迟**（写入为 0、预算为 0）。
- **后台化不影响写延迟**：`MaybeScheduleCompaction` 只做"判断 + 投任务"，真正的 IO 在后台线程。`~DBImpl` 通过 `active_compactions_` + barrier 任务确保析构时无任务在访问 `this`。

---

## 三、模块分层

```
        ┌─────────────────────────────────────┐
        │  db.h / db_impl.h   （对外 API）      │  ← Snapshot / ReadOptions 在此
        │  db.cpp / db_iterator.h              │
        ├─────────────────────────────────────┤
        │  version_set.h/cpp                  │  ← Version / VersionEdit / MANIFEST / TableCache
        │  table.h / table.cpp                │  ← SSTable 读写
        │  filter_policy.h/cc                 │
        ├─────────────────────────────────────┤
        │  memtable.h / skiplist.h / arena.h  │  ← 内存表
        │  block.h / block.cpp                │  ← 块格式
        ├─────────────────────────────────────┤
        │  log_writer / log_reader            │  ← WAL 与 MANIFEST 共用的记录格式
        │  internal_key / coding / slice      │
        ├─────────────────────────────────────┤
        │  env.h / env_posix.cpp              │  ← OS 抽象（文件 / 线程 / 锁）
        │  status / crc32c                    │
        └─────────────────────────────────────┘
```

**依赖方向严格自上而下**，无反向依赖。`test_util` 是例外——它被 `db.cpp` 引用以提供崩溃注入点，但功能上属于测试设施。

---

## 四、文件格式规格

### 4.1 块（Block）—— SSTable 与 WAL 的共同基础

```
┌──────────────────────────────┬──────────┬────────────┐
│ entry × N（前缀压缩）         │ restart  │ num_restarts│
│                              │ 数组      │  (4B)      │
└──────────────────────────────┴──────────┴────────────┘
                                    每项 4B 偏移量
```

每条 entry：

```
┌─────────────┬──────────────┬───────────────┬─────────┐
│ varint32    │ varint32     │ varint32      │ 剩余字节 │
│ shared      │ non_shared   │ value_length  │ 非共享部分│
└─────────────┴──────────────┴───────────────┴─────────┘
   完整 key = 前一条 key 的前 shared 字节 + 本条的 (non_shared + value_length) 字节
```

重启点间隔默认 16。**它存在的意义不只是加速正向查找**：前缀压缩让"回退一步"无法直接解析（entry 头只存与**前**一条的共享字节数），反向定位必须"从最近的重启点重扫一小段、取其中最后一条"。没有重启点，反向一步就要从块首重扫，退化成 O(n)。

### 4.2 块尾与校验

```
块内容 ┬─ crc32c(4B) ─┬─ type(1B) ─┐
       └──────────────┴────────────┘  kBlockTrailerSize = 5
```

CRC 覆盖块内容，**不含 crc 自身**。`ReadBlock` 会剥掉这 5 字节，因此交给 `Block` 的 `Slice` 是 trailer-free 的。

### 4.3 SSTable（`.ldb`）

```
┌──────────────┬──────────────┬──────────────┬──────────────┐
│ data block × N│  index block │ metaindex blk│ filter block │
└──────────────┴──────────────┴──────────────┴──────────────┘
┌──────────────────────────────────────────────────────────┐
│ footer (48B)                                                │
└──────────────────────────────────────────────────────────┘
```

- **index block**：每条 = `internal_key`（分隔符）→ `BlockHandle`
- **metaindex block**：当前只有 `filter.*` 一项
- **BlockHandle** = `varint64 offset` + `varint64 size`（各至多 10 字节）
- **footer**：`meta_index_handle` + `index_handle` + `magic(8B)` = 48 字节
- **magic** = `0xdb4775248b80fb57`

⚠️ 解码 footer 时 `BlockHandle::DecodeFrom` 必须**原地消耗 Slice**（传 `Slice*`）。若按值传参，`rest` 不会前进，index handle 会从 meta handle 的字节开始解，索引块定位错位——这是一个曾真实发生过的 bug。

### 4.4 记录格式（WAL 与 MANIFEST 共用）

文件切成 32KB 块，每条记录头 7 字节：

```
┌─────────────┬─────────────┬─────────────┐
│ crc32c(4B)  │ length(2B)  │ type(1B)    │
└─────────────┴─────────────┴─────────────┘
   crc 覆盖 type 字节 + payload，不含 length 本身
```

`type` 取 `FULL(1) / FIRST(2) / MIDDLE(3) / LAST(4)`。放不下的记录切分成分片，下一片从新块开头继续。**记录小于块头时用全零填满块尾，读到时跳过。**

MANIFEST 存 `VersionEdit`，WAL 存 `WriteBatch`。

### 4.5 内部键

```
internal_key = user_key ‖ sequence(7B, 大端) ‖ type(1B)     共 8 字节后缀
```

⚠️ **不能用 `memcmp` 比较内部键**。比较器必须是：先比 `user_key`（用用户比较器），相等则 `seq` **降序**（大的在前），最后比 type。

压缩时缩短 key 有个陷阱：`FindShortestSeparator` / `FindShortSuccessor` **只能压缩 `user_key` 部分**，再重新拼回合法的 8 字节后缀。直接对整个 internal key 做压缩会产生长度 < 8 的非法键，后续解码即崩。

### 4.6 库目录

```
<dbname>/
├── MANIFEST        当前版本记录
├── LOCK            flock 排他锁（内核级，进程退出自动释放）
├── 000005.log      WAL
├── 000007.ldb      SSTable
└── ...
```

编号由 `VersionSet::NewFileNumber()` 统一分配。

---

## 五、W4~W9 的关键决策

W1~W3 见 `design-notes.md`。以下记录后续阶段中**容易被后来者推翻或误解**的决策。

| 阶段 | 决策 | 为什么 |
|---|---|---|
| W4 | MANIFEST 持久化 `last_sequence` | flush 后旧 WAL 被删、新 WAL 为空，只靠重放 WAL 会让快照回退到 0，**已 flush 的全部数据因 `seq > 0` 而不可见** |
| W4 | `VersionSet` 对 `current_` 持所有权引用 | `TryRef` 在 `refs_==0` 时返回 false，读路径会死循环 |
| W4 | `ApproximateMemoryUsage` 改为统计记录字节数 | 原先返回 Arena 预留量，导致阈值恒被突破、**每次 Put 都 flush** |
| W5 | `NewIterator` 不自动 `SeekToFirst` | 初始位置"无效"由调用方显式选择，职责清晰，也避免给反向遍历埋语义歧义 |
| W6 | TableCache 加引用计数 | compaction `delete` 会把并发读者手里的裸指针变成悬垂指针 |
| W7 | `MaybeScheduleCompaction` 只投任务 | 写路径不做 IO，压缩不阻塞写入 |
| W7 | `WaitForBackgroundCompaction` 忙等而非条件变量 | 条件变量需在 `~DBImpl` 销毁，而后台线程可能恰好在 `notify_all`，TSAN 会判为竞争 |
| W8 | `Options::filter_policy` 默认指向进程级单例 | 原先每次构造 `Options` 都 `new` 一个策略对象，**每次都泄漏**（ASAN 报 11 处） |
| W8 | 快照点统一截断到 `last_sequence_` | 见 §2.1，否则会读到未提交数据 |
| W9 | 锁用 `flock` 而非 LOCK 文件标志 | 内核级、崩溃自动释放、`LOCK_NB` 不卡住、加锁与"是否占用"是同一原子操作 |

**W9 的已知局限**：`flock` 锁的是"打开文件描述"而非进程，同进程内两次 `Open` 拦不住。这是 flock 的既定语义，真实风险来自多进程。

---

## 六、配置项

| 字段 | 默认 | 说明 |
|---|---|---|
| `create_if_missing` | `false` | ⚠️ **当前未实现**，打开不存在的库会静默创建（见 `code-review.md` W6） |
| `error_if_exists` | `false` | 目录已存在则报错 |
| `write_buffer_size` | 4MB | MemTable 超过则 flush |
| `max_table_cache_bytes` | 64MB | SSTable 缓存上限，0 = 不淘汰 |
| `block_size` | 4KB | data block 目标大小 |
| `l0_compaction_trigger` | 4 | L0 文件数达此值触发压缩 |
| `max_level_bytes` | 8MB | L1 容量 |
| `max_level_bytes_multiplier` | 10 | 每层容量倍率，⚠️ 设为 0 会整数除零 |
| `max_num_levels` | 7 | ⚠️ 末层压缩会产出第 8 层，该层永不参与压缩 |
| `max_compaction_file_size` | 2MB | 单个 compaction 输出文件目标大小 |
| `compaction_max_write_amplification` | 1 | 压缩输出 ≤ 写入量 × 该系数 |
| `compaction_backlog_factor` | 4 | L0 积压超过 `trigger × factor` 时豁免限流 |

---

## 七、已知限制

以下均为**已确认存在**的缺陷。优先级 1~5 的条目已在审查后修复，剩余项按"不做的后果"排序。完整分析见 [`code-review.md`](code-review.md)。

| 限制 | 后果 |
|---|---|
| WAL 滚动与 compaction 输出均未检查 `Close()` | 输出文件 `Close` 失败会被忽略，可能把不完整文件登记进 MANIFEST |
| MANIFEST 只追加不压缩 | 重放时间随运行时长线性上升 |
| L1→L2 整层参与 compaction | 层大时写放大偏高（纯性能，非正确性） |
| 13 处 `DeleteFile` 未检查返回值 | unlink 失败导致孤儿文件堆积（下次启动会清理） |
| 严格警告 36 处（`-Wsign-conversion` 为主） | 目前都不是 bug，但属该清理的欠账 |

**已修复**（曾是高危，现已有回归测试覆盖）：`CompactLevel` 悬垂指针、`NewIterator` 引用建立太晚、`log::Reader` 无法区分损坏与 EOF、`Block` 重启点数组无下界校验、`metaindex` 损坏被吞成 OK、层号 off-by-one、`create_if_missing` 未实现、`max_level_bytes_multiplier=0` 整数除零、`Get` 丢弃 MemTable Corruption、`InsertInto` 忽略 `Iterate` 失败。

**架构层面的已知空白**（尚未实现，不算缺陷）：无网络文件系统适配；无块缓存（每次点查都重读索引块）；`Logger` 接口存在但未接入；无指标导出。

**测试盲区**（已部分补上）：`IteratorSurvivesConcurrentFlushAndCompaction` 打开了"迭代器存活期间并发 flush/compaction"的交叉窗口，两处 UAF 正是因此长期未被发现。同类盲区仍存在于"WAL 记录写到一半"（尾部截断）——W9 覆盖了"进程被杀"，未覆盖"记录写到一半"。