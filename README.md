# TinyStore

一个从零实现的 **LSM-Tree 存储引擎**，用于系统性学习 C++ 系统级编程。

目标不是"做一个能用的数据库"，而是**把内存、并发、文件 I/O、性能工程这四类
硬骨头逐个啃一遍**，并且每个知识点都能被追问三层。

> **文档**
>
> | 文档 | 内容 |
> |---|---|
> | [`docs/architecture.md`](docs/architecture.md) | **架构总览**：三条主路径的数据流、模块分层、**文件格式精确规格**、W4~W9 关键决策、配置项、已知限制 |
> | [`docs/design-notes.md`](docs/design-notes.md) | **设计取舍与踩坑记录**（W1~W3，含实测性能数据与 TSAN 抓到的真实 bug）——面试素材 |
> | [`docs/code-review.md`](docs/code-review.md) | **代码审查报告**：已验证的缺陷、证伪记录、修复优先级 |

---

## 环境

| 项目 | 版本 |
|---|---|
| 发行版 | Ubuntu 24.04 (WSL2) |
| 编译器 | GCC 13.3 / Clang 18 |
| CMake | 3.28+ |
| 构建器 | Ninja |
| 测试框架 | GoogleTest 1.14 |
| 基准框架 | Google Benchmark 1.8 |

一键安装依赖：
```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build gdb pkg-config \
     libgtest-dev libgmock-dev libbenchmark-dev \
     libsnappy-dev libzstd-dev \
     clang clang-tidy clang-format valgrind
```

> **注意**：代码必须放在 WSL 的 ext4 文件系统（如 `~/projects/TinyStore`），
> 不要放在 `/mnt/c` 或 `/mnt/g` 下 —— NTFS 通过 9P 挂载会导致 `fsync`
> 语义异常、随机写性能下降一个数量级，benchmark 数据将完全失真。

---

## 构建与测试

```bash
# 常规构建（RelWithDebInfo：带优化 + 调试符号，便于 perf 剖析）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure

# AddressSanitizer（抓越界 / UAF / 内存泄漏）
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTINYSTORE_ENABLE_ASAN=ON
cmake --build build-asan && ctest --test-dir build-asan

# ThreadSanitizer（抓 data race）
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTINYSTORE_ENABLE_TSAN=ON
cmake --build build-tsan && ctest --test-dir build-tsan
# 在 WSL2 上 TSAN 需要解除地址空间布局限制，且一次跑多个进程会互相干扰，
# 因此逐个执行（Linux 原生环境直接用上面的 ctest 即可）：
bash tools/run_tsan_wsl.sh

# 严格警告（提交前跑一次，专项清理整型转换问题）
cmake -B build-strict -G Ninja -DTINYSTORE_STRICT_WARNINGS=ON

# 基准测试
./build/bin/arena_bench
```

代码格式化与静态检查：
```bash
find src include tests benchmarks -name '*.cpp' -o -name '*.h' | xargs clang-format -i
clang-tidy -p build src/*.cpp
```

---

## 目录结构

```
TinyStore/
├── include/tinystore/     # 公开头文件
│   ├── slice.h            # 非拥有字节视图（热路径零拷贝的地基）
│   ├── status.h           # 错误处理（不用异常）
│   ├── coding.h           # 定长小端 / Varint 编解码
│   ├── comparator.h       # key 排序规则抽象
│   ├── arena.h            # 内存池（bump allocator）
│   ├── internal_key.h     # 内部键编码（MVCC 的命门）
│   └── env.h              # OS 抽象层（文件 / 线程 / 时钟）
├── src/                   # 实现
├── tests/                 # 单元测试（每个模块一个独立可执行文件）
├── benchmarks/            # 微基准测试
├── tools/                 # 开发辅助脚本（TSAN 在 WSL2 下的运行方式）
├── cmake/                 # 编译选项与 Sanitizer 配置
└── docs/
    ├── architecture.md    # 架构总览：数据流 / 文件格式 / 关键决策
    ├── design-notes.md    # 设计取舍与踩坑记录（W1~W3，面试素材）
    └── code-review.md     # 代码审查报告
```

---

## 已知限制

第一轮代码审查发现的 15 项缺陷中，优先级 1~5 的部分**已全部修复**并补了回归测试。剩余项按"不做的后果"排序，完整分析见 [`docs/code-review.md`](docs/code-review.md)。

| 限制 | 后果 |
|---|---|
| WAL 滚动与 compaction 输出均未检查 `Close()` | 输出文件 `Close` 失败会被忽略，可能把不完整文件登记进 MANIFEST |
| MANIFEST 只追加不压缩 | 重放时间随运行时长线性上升 |
| L1→L2 整层参与 compaction | 层大时写放大偏高（纯性能，非正确性） |
| 13 处 `DeleteFile` 未检查返回值 | unlink 失败导致孤儿文件堆积（下次启动会清理） |

**已修复的高危项**（均有回归测试）：`CompactLevel` 悬垂指针、`NewIterator` 引用建立太晚、`log::Reader` 无法区分损坏与 EOF、`Block` 重启点数组无下界校验、`metaindex` 损坏被吞成 OK、层号 off-by-one、`create_if_missing` 未实现、`max_level_bytes_multiplier=0` 整数除零、`Get` 丢弃 MemTable Corruption、`InsertInto` 忽略 `Iterate` 失败。

修复过程中还暴露一个值得记录的陷阱：把 `Ref()` 移进锁内以修 UAF 时，若适配器构造时**也** `Ref()`，就变成两次 Ref、一次 Unref，功能测试全绿但永久泄漏一个引用——只有 LeakSanitizer 在进程退出时才看得见。见 [`docs/code-review.md`](docs/code-review.md) 的"修复过程中发现的新问题"。

---

## 路线图

| 周 | 主题 | 状态 |
|---|---|---|
| **W1** | 工程地基 + `Slice` / `Status` / `Coding` / `Comparator` / `Arena` / `InternalKey` / `Env` | ✅ 完成 |
| **W2** | `SkipList` + `MemTable` + `WriteBatch` + `WAL` + `CRC32C` | ✅ 完成 |
| **W3** | DB 写路径 + Group Commit + 无锁快照读 | ✅ 完成 |
| **W4** | 读路径 + 持久化：`Block` / `SSTable` / `BloomFilter` / `Flush` / `Version` / `VersionSet` / `MANIFEST` / 崩溃恢复 | ✅ 完成 |
| **fix(W4)** | 代码审查缺陷修复：静默丢数据、RCU use-after-free、整数溢出绕过等 | ✅ 完成 |
| **W5** | 迭代器：`DB::NewIterator` + SSTable 遍历 + 跨源归并 + MVCC 过滤 + 快照读 | ✅ 完成 |
| **W6** | Compaction：Leveled 策略 + 墓碑回收 + TableCache 引用计数 | ✅ 完成 |
| **W7** | Compaction 后台化：`Env::Schedule` + 生命周期安全等待 | ✅ 完成 |
| **W8** | P1 收口：TableCache LRU 淘汰 + Snapshot 句柄 API + 迭代器反向遍历 + Compaction 优先级与限流 | ✅ 完成 |
| **W9** | 可靠性：库级排他锁 + 崩溃恢复测试（崩溃注入点 + fork/exec） | ✅ 完成 |

> 说明：原规划把「读路径 / BloomFilter」列为 W5、「Flush / Version / MANIFEST」列为 W6。
> 实际执行时这两块内容合并进了 W4 一次提交（原 W5、W6 的条目已不再单列）。
> W2 也并入了原 W3 的 WAL 部分。W8 收口了原「后续计划」里的全部 P1 项。

### 后续计划

| 优先级 | 主题 | 动机 / 现状缺口 |
|---|---|---|
| **P2** | **MANIFEST 快照与压缩** | MANIFEST 目前只追加、从不压缩。W6 引入 compaction 后增删记录变频繁，长时间运行会无限增长，重放时间随之线性上升 |
| **P2** | **多版本压缩（Compaction 选点优化）** | W8 已能按压力选层，但 L1→L2 仍是"整层全部文件参与"。真实负载下应改为按字节预算滚动选点，避免一次归并搬运整个层 |
| **P2** | **WAL 半条记录的处理** | W9 已用 fork/exec 验证了"进程被杀"的恢复路径。尚未覆盖"记录写到一半"（尾部 CRC 或 payload 截断）——`log::Reader` 对残缺尾记录应当静默丢弃，但这条分支仍无测试 |
| **P3** | **性能与可观测**：Block 缓存、统计信息、`Logger` 落地 | `ApproximateOffsetOf` 当前是简化版；`Logger` 接口存在但未接入。W8 已加了缓存条目数/字节数与写放大计数，可作为指标体系的起点 |
| **P3** | **反向迭代的按键范围扫描** | W8 的 `Prev` 每次都要扫过该 user_key 的全部版本（总代价 O(总 entry 数)）。数据量上来后可加"范围反向扫描"接口，只在块边界回扫 |
| **P4** | 可选扩展：`epoll` Reactor + RESP 协议、Raft 复制 | 网络层与分布式复制，属于加分项，不影响存储引擎主线 |

### 库级排他锁（W9）

`DB::Open` 会先抢 `<目录>/LOCK` 的 `flock`，抢不到直接返回错误。原因是两个进程同时打开同一目录时，各自维护独立的 MemTable 与 VersionSet，会并发写同一个 MANIFEST、互删对方的 SSTable——**全程无任何报错**，只是一路安静地把数据写坏，比崩溃更难排查。

选 `flock` 而不是"创建一个 LOCK 文件当标志"的原因：

- 进程退出（含 `kill -9`）时内核自动释放，不会留下永久死锁；
- `LOCK_NB` 拿不到锁立刻报错，而非让调用方莫名卡住；
- 加锁与"是否已被占用"是同一个原子操作，不存在"检查-再打开"的竞态窗口。

**已知局限**：`flock` 锁的是"打开文件描述"而非进程，同一进程用两个不同 fd 打开同一文件会各自获得自己的锁。因此同进程内的重复 `Open` 不会被拦下——这是 flock 的既定语义，真正的风险来自多进程并发。相应地，锁的测试也用两个真实进程来验证。

### 崩溃恢复测试（`crash_test`）

用 `fork + exec` 制造真实的"进程被杀"，验证**已返回 OK 的写入在崩溃后绝不丢失**：

| 崩溃点 | 考验的边界 |
|---|---|
| `kCrashPointAfterWalSync` | WAL 已 fsync 但内存视图未更新 → 恢复必须靠 WAL 重放找回 |
| `kCrashPointAfterFlushSstWrite` | SSTable 已落盘但 MANIFEST 未登记 → 该文件是孤儿，必须被清理且数据仍能从旧 WAL 重放 |
| `kCrashPointAfterFlushCommit` | MANIFEST 已提交但旧 WAL 未删 → 数据"两头都有"，恢复必须容忍重叠而非报错 |
| `kCrashPointAfterCompactionOutput` | compaction 输出已落盘但未提交 → 源文件仍必须有效，否则**唯一持有数据的文件被当孤儿删掉** |

两个必须用 `fork + exec` 而非单纯 `fork` 的原因（踩过才知道）：

1. **Env 的后台线程池是进程级单例**。`fork` 只复制调用线程，子进程里线程池对象还在但执行线程不存在 → 子进程析构 DB 时 `WaitForBackgroundCompaction()` 投的 barrier 任务永远没人执行，直接死锁；更糟的是 compaction 根本不会跑，崩溃点打不到。
2. **`setenv` 不能用于父子传参**。它会调 `malloc`，而 fork 后子进程继承了其他线程的 malloc 锁状态。改用 `argv`（fork 前就备好字符串）+ `execv`，全程只读访问。

TSAN 下不运行此测试：TSAN 无法跨 fork 跟踪访问，会产生大量与被测代码无关的误报。它验证的是**持久化协议**（文件与 MANIFEST 的先后顺序），本身不依赖内存序。

---

## W1 质量状态

| 检查项 | 结果 |
|---|---|
| 单元测试 | ✅ 7 / 7 通过 |
| ASAN（含 Debug assert） | ✅ 7 / 7 通过 |
| TSAN | ✅ 7 / 7 通过，零告警 |
| 编译警告 | ✅ 零警告（`-Wall -Wextra -Wpedantic -Wshadow` 等） |

### 第一组性能数据

10,000 次分配，WSL2 / GCC 13.3：

| 尺寸 | Arena | malloc/free | new/delete |
|---|---|---|---|
| 32 B | **19.0 μs** | 298.1 μs | 232.0 μs |
| 128 B | **340.1 μs** | 559.8 μs | 428.9 μs |
| 1024 B | 3,585.8 μs | **3,366.7 μs** | **2,291.7 μs** |

小对象场景 Arena 快 15.7 倍；但 1024 字节时反而更慢。
原因分析与工程启示见 `docs/design-notes.md`。
