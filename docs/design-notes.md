# TinyStore 设计笔记

> 这份文档是项目的**核心资产**，重要性高于代码本身。
> 面试时讲项目的深度，几乎全部来自这里记录的"为什么这么做"以及踩过的坑。

---

## 一、项目定位

从零实现一个 **LSM-Tree 存储引擎**，目标不是做一个能用的数据库，
而是**系统性地覆盖 C++ 系统级编程的核心知识点**，并且每个知识点都能被追问三层。

| 维度 | 覆盖内容 |
|---|---|
| 内存 | 内存池、对象布局、缓存行、生命周期管理、placement new |
| 并发 | 锁粒度演进、六种 memory_order、条件变量、无锁结构 |
| 文件 I/O | 系统调用语义、fsync、pread/pwrite、崩溃一致性 |
| 性能工程 | benchmark、火焰图、数据驱动的优化叙事 |
| 正确性 | gtest、ASAN/TSAN/UBSAN、故障注入 |

---

## 二、七个"架构锚点"（第 1 周定死，后续不可变）

这些决策本身不复杂，但**改它们的成本等于重写整个项目**。
它们的共同点是：一旦散落到文件格式的字节布局里、或散落到成百上千个函数签名里，
再想改就是全量重构。

| # | 决策 | 若不现在定死，后续代价 |
|---|---|---|
| 1 | 用 `Slice` 而非 `std::string` 传递所有 key/value | 全项目签名改动，热路径性能无法回退 |
| 2 | `InternalKey` 编码（user_key + 7B seq + 1B type） | 加 MVCC 时 MemTable / SSTable / Comparator / Compaction **全量重写** |
| 3 | 统一的 `Status` 错误处理（不用异常） | 接口不一致，错误信息丢失 |
| 4 | `Env` 抽象层（文件 / 线程 / 时钟） | 无法做故障注入、确定性测试、分布式扩展 |
| 5 | WAL 以 `WriteBatch` 为记录单位 | 加事务时 WAL 格式与恢复逻辑全部重写 |
| 6 | `Version` / `VersionSet` / `MANIFEST` 版本管理 | Compaction 无法原子切换，并发读写正确性无法保证 |
| 7 | 引用计数管理 MemTable / SSTable 生命周期 | flush / compact / read 三条线并发时 UAF |

**设计原则**：用第 1 周的"过度设计"，换取后续 9 周的**纯增量开发**。

---

## 三、W1 各组件的设计取舍

### 3.1 `Slice` —— 非拥有视图

**决策**：手写 `Slice{const char*, size_t}`，而非直接用 `std::string_view`。

**理由**：
1. 生产环境应该用 `std::string_view`，这里手写是教学目的 ——
   亲手实现一遍才能真正理解"悬垂引用"的边界；
2. 需要自定义比较语义（memcmp 字节序），以及后续挂载 CRC、序列化等工具函数。

**关键约束**：`Slice` 不延长被指对象的生命周期。
```cpp
Slice Bad()  { std::string s = "hi"; return Slice(s); }   // !!! 悬垂
void  Good(std::string* buf, Slice* out) { *out = Slice(*buf); }  // 正确
```

### 3.2 `Status` —— 为什么不用异常

| 理由 | 说明 |
|---|---|
| 性能模型可预测 | 异常需要 unwind 表，增加二进制体积和 icache 占用 |
| 错误是常态 | `NotFound` 是正常业务结果，不是"异常" |
| 与系统调用一致 | read/write 用返回码 + errno，无需做异常转换 |
| 可强制检查 | 配合 `[[nodiscard]]`，漏检时编译器报警 |

**内存布局**（`OK` 状态零成本）：
```
state_ == nullptr                        -> OK，sizeof(Status) == 8
state_ -> [u32 msg_len][u8 code][msg...] -> 错误，慢路径才分配
```

### 3.3 `Coding` —— 字节格式

- 定长 32/64 位用**小端序**；
- 实现上**刻意逐字节移位而非 memcpy**，换取与宿主机字节序无关的可移植性。
  若未来 perf 显示这里是热点，可加 `#if` 分支走 memcpy 快路径。
- **经典陷阱**：`char` 在 x86 上是有符号的，`static_cast<uint32_t>(ptr[i])`
  对 >= 0x80 的字节会符号扩展成 `0xffffff80`。必须先转 `unsigned char`。
  `coding_test.cpp` 里的 `Fixed32HighBitDoesNotSignExtend` 专门守着这个 bug。

### 3.4 `Arena` —— bump allocator

**解决的问题**：MemTable 每秒数十万次小对象分配，用 malloc 会遭遇
分配慢、内存碎片、逐个释放代价高三重问题。

**核心契约**：
- 只提供 `Allocate`，没有 `Free`；
- **析构时不调用任何对象的析构函数** —— 因此只能放 trivially destructible 的对象；
- 绝不能把 `std::string` / `std::vector` 放进 Arena（会永久泄漏）。

**`MemoryUsage()` 为什么用 `memory_order_relaxed`**：
它会被后台 flush 线程读取而由前台写线程更新，存在跨线程访问，
因此必须是 atomic 以避免 data race。但用 relaxed 就够了 ——
它只是近似统计量，读到一个稍旧值最多让 flush 晚几毫秒触发。
用 seq_cst 会白白插入内存屏障。**这是"按需求选最弱内存序"的典型例子。**

**ASAN 的局限**：ASAN 不知道块内被切成多少个小对象，无法检测 Arena 内部越界。
需要用 `__sanitizer_annotate_contiguous_container` 手动告知边界，
计划在 W8 调试阶段补上。

### 3.5 `InternalKey` —— 整个引擎的命门

编码格式：
```
| user_key (变长) | sequence (7 字节) | type (1 字节) |
                  └────── 固定 8 字节后缀 ──────────┘
```

排序规则：
1. user_key **升序**（用用户 Comparator）
2. user_key 相同时，sequence **降序**（新的在前）

这个规则带来关键性质：**对某个 key 查找时，遇到的第一个匹配项就是最新版本**。
这就是 MVCC 快照读的实现基础 —— "读 seq <= 100 的值" 等价于
"找第一个 seq <= 100 的项"。

#### ⚠️ 最隐蔽的陷阱：后缀不能用 memcmp

小端序下，"最低有效字节在最低地址"，而 memcmp 从低地址比起。反例：

```
A = 0x0100 (256)   小端字节: [00][01]
B = 0x00FF (255)   小端字节: [FF][00]

memcmp  : 首字节 0x00 < 0xFF  ->  A < B   （错误！）
数值比较: 256 > 255           ->  A > B   （正确）
```

所以正确做法是：user_key 用 memcmp，后缀必须 `DecodeFixed64` 成数值再比，然后取反实现降序。
`internal_key_test.cpp` 的 `SuffixComparisonMustBeNumericNotMemcmp` 专门守着这点。

> 写测试时我自己在 `ParseRejectsUnknownValueType` 里就踩了这个坑 ——
> 想篡改 type 字段却改了 `encoded.back()`（那是 sequence 的高位字节），
> 正确下标是 `encoded.size() - 8`。

### 3.6 `Env` —— OS 抽象层

三个不可替代的价值：
1. **故障注入**：写 TestEnv 在第 N 次 Write 时返回 IOError，
   验证"掉电后数据不丢且可恢复" —— 这是存储引擎最核心的正确性保证，
   在普通测试里根本无法构造这种场景。
2. **确定性测试**：Compaction 触发依赖时间。把时钟抽象后可用 MockEnv
   手动推进时间，把依赖 sleep 的测试变成毫秒级的确定性单元测试。
3. **未来扩展**：做 Raft 时，"写日志"要从"写本地文件"变成"写本地 + 复制给多数派"。
   若 WAL 只依赖 Env 接口，这个改造只是换一个实现，而非重写 WAL。

关键实现细节：
- `RandomAccessFile::Read` 用 `pread` 而非 `lseek + read`：
  pread 不修改文件偏移量，**天然线程安全**，SSTable 的多线程并发读无需加锁。
- `WritableFile::Append` 必须**循环处理短写**（short write）：
  `write()` 不保证一次写完，磁盘满、信号中断、NFS 都可能短写。
- `PosixEnv::~PosixEnv` 必须 join 后台线程：
  否则正在 flush 的 MemTable 会丢失，而这些数据已经向客户端返回过"写入成功"。
- `Schedule` 在 shutting_down 时**回退到调用线程同步执行**，
  避免任务被静默丢弃导致数据丢失。
- `Env::Default()` **刻意泄漏单例**（`static PosixEnv* p = new PosixEnv()`）：
  避免静态析构顺序问题，以及退出时 join 后台线程可能引发的挂起。

---

## 四、性能基线（W1）

测试环境：WSL2 / Ubuntu 24.04 / GCC 13.3 / 16 核。
每组 10,000 次分配，取 Google Benchmark 稳定值。

| 分配尺寸 | Arena | malloc/free | new/delete | Arena 相对 malloc |
|---|---|---|---|---|
| 32 B   | **19.0 μs**  | 298.1 μs | 232.0 μs | **15.7× 快** |
| 128 B  | **340.1 μs** | 559.8 μs | 428.9 μs | 1.65× 快 |
| 1024 B | 3,585.8 μs   | **3,366.7 μs** | **2,291.7 μs** | **0.94×（Arena 更慢）** |

### 结论与分析

1. **小对象场景 Arena 优势巨大**（32 字节时 15.7 倍）。
   这正是 MemTable 节点的典型尺寸，选择 Arena 是正确的。

2. **1024 字节时 Arena 反而最慢**，原因是两点：
   - Arena 每个 4KB 块只能放 4 个 1KB 对象，10,000 次分配要新开 2,500 个块，
     等于 2,500 次 `new char[]`，退化成了"用 malloc 间接分配"；
   - malloc/free 反复分配释放**同一尺寸**的块，会复用刚刚释放的内存，
     缓存是热的；而 Arena 每轮都要触碰 10MB 全新冷页。

3. **工程启示**：Arena 的优势来自"批量分配 + 整体释放"，
   只在**对象小、数量多、生命周期一致**的场景成立。
   这不是"Arena 一定比 malloc 快"，而是**按场景选型**。
   （这正是面试时比单纯报数字更有价值的部分。）

> 注意：Google Benchmark 报了 `Library was built as DEBUG` 警告，
> 这是链接的预编译 benchmark 库不带 NDEBUG 所致，不影响相对比较的结论。
> W8 会用 LTO + 完整 Release 重新采集一遍。

---

## 五、质量门禁

| 检查项 | 命令 | 状态 |
|---|---|---|
| 单元测试 | `ctest --test-dir build` | ✅ 13/13 通过 |
| 内存安全（ASAN + Debug assert） | `ctest --test-dir build-asan` | ✅ 13/13 通过 |
| 并发安全（TSAN） | `ctest --test-dir build-tsan` | ✅ 13/13 通过 |
| 编译警告 | `-Wall -Wextra -Wpedantic -Wshadow ...` | ✅ 零警告 |

Debug 构建下所有 `assert` 均生效，意味着 Arena 对齐、InternalKey 长度、
后缀解析等不变式都经过了运行时校验。W2/W3 累计 13 个测试目标
（`slice_test` / `coding_test` / `status_test` / `comparator_test` / `arena_test` /
`internal_key_test` / `env_test` / `crc32c_test` / `skiplist_test` / `memtable_test` /
`write_batch_test` / `log_test` / `db_test`）在 none / asan / tsan 三种配置下全部通过；
其中 `db_test` 的并发用例（8 线程 × 200 次写 + 读）专门给 TSAN 提供
"Group Commit + 无锁读" 的数据竞争覆盖。

### TSAN 在本机 WSL2 下的状态

早期在另一台 WSL 镜像上遇到过：
```
FATAL: ThreadSanitizer: unexpected memory mapping
```
原因是 TSAN 要求进程地址空间落在特定范围内，与 WSL2 内核的
地址随机化（`randomize_va_space=2`）冲突。本机 `Ubuntu-24.04` 镜像上
**该问题不复现**，TSAN 构建可直接 `ctest` 跑通（W2 全绿）。

若日后换镜像又触发，已验证可用的解法（无需改系统配置，仅对单个进程关闭 ASLR）：
```bash
setarch x86_64 -R ./build-tsan/bin/<test>
```
可把它包进 CMake：让 `add_test` 的命令变成 `setarch -R <exe>`。
其它备选：
```bash
sudo sysctl -w vm.mmap_rnd_bits=28       # WSL 重启后失效
valgrind --tool=helgrind ./build/bin/<test>   # 慢但一定能跑
```

### TSAN 抓到的第一个真实 bug（W1）

在 `EnvTest.ScheduleRunsTaskOnBackgroundThread` 中，
`cv.notify_one()` 写在了互斥锁作用域**之外**：

```cpp
{  std::lock_guard<std::mutex> lock(mu);  done = true;  }
cv.notify_one();   // ← 此时锁已释放
```

TSAN 报告：
```
Write of size 8 by main thread:  pthread_cond_destroy   (析构 cv)
Previous read of size 8 by T1:   pthread_cond_signal    (通知中)
```

**根因**：通知线程可能仍在 `notify_one()` 内部执行，
而主线程已从 `wait_for` 返回并析构了栈上的 `cv` —— 未定义行为。

**修复**：把 `notify_one()` 移进锁的作用域内。
这样主线程从 `wait_for` 返回必须先重新获得互斥锁，
而锁只有在通知线程完成 notify 并解锁后才可得，
从而建立了必要的 happens-before 关系。

**代价**：持锁通知会让被唤醒的线程立刻阻塞在互斥锁上
（"hurry up and wait"，多一次上下文切换）。
生产代码里若 `cv` 的生命周期长于所有通知者，可以解锁后再通知以换取这点性能；
但本例中 `cv` 是栈上局部变量，正确性优先。

> 这个 bug 值得记住：它是"条件变量 + 栈上局部变量"组合的经典陷阱，
> 在测试代码里尤其常见，而且不加 TSAN 几乎不可能发现。

---

## 六、W2 设计取舍（MemTable / SkipList / WriteBatch / WAL）

W2 的目标很朴素：**让引擎第一次"能写入并持久化"**。
一个 `WriteBatch` 先写进内存 `MemTable`（SkipList），再顺序追加到 WAL，
崩溃后靠 WAL 重放恢复。这一周把"写路径"完整打通。

### 6.1 `SkipList` —— 手写，缓存友好的内联节点

**为什么不用 `std::map` / `std::unordered_map`**：
- 红黑树节点前向/后向指针 + 颜色 + 堆分配，内存碎片严重；
- 哈希表要算 hash、解决冲突，且无法做"范围扫描"（MemTable 必须支持 `Seek`）；
- LSM-Tree 的 MemTable 读多写多、需要有序遍历，SkipList 是教科书级选择。

**节点内存布局（关键优化）**：
```cpp
struct Node {
  const Key key;
  Node* next_[1];   // 柔性数组：实际长度 = height
};
```
`key` 与 `next_` 放在**同一块内存**里，一次 cache line 命中就能拿到
key 和指针，对跳表"自上而下再水平"的访问模式极其友好。
节点用 placement new 在 Arena 上分配，`height` 决定 `next_` 的真实长度
（`sizeof(Node) + (height-1)*sizeof(Node*)`）。

**层高随机化**：每层以 1/4 概率再升一层（`rnd() < (1<<31)/4`），
期望层高 ≈ 1/(1-1/4) = 1.33，最大 12 层可覆盖 2^24 个节点。
用确定性 LCG（linear congruential generator）而非 `std::rand()`：
测试可复现，排查并发/崩溃问题时不背"随机种子"的额外变量。

**无锁读**：每个 `next_[i]` 是 `std::atomic<Node*>`，`FindGreaterOrEqual`
全程只做 `load(std::memory_order_acquire)`，**完全不加锁**。
写线程在插入时先 `NoBarrier_SetNext`（发布前用 relaxed 写指针），
最后用 `release` 把新节点"挂"到前一层的 `next_`，读线程靠 acquire 看到。
本阶段 MemTable 仍由单个写锁保护（见 6.2），SkipList 的并发语义
为 W3 放开写锁、W8 后台 Compaction 并发读打好基础。

> 踩坑：Clang 会对 GNU 柔性数组扩展报警（`-Wgnu-flexible-array-extensions`），
> 用 `#pragma` 仅在 Clang 下抑制；GCC 下该写法本就合法，不影响零警告目标。

### 6.2 `MemTable` —— SkipList + Arena + 引用计数

**三个核心机制**：

1. **引用计数（`Ref` / `Unref`）**：MemTable 同时被三条线持有 ——
   前台写线程（正在写）、后台 flush 线程（正在落盘）、读线程（快照读）。
   用 `atomic<int> refs_` 计数，归零时 `delete this`。这正是 W1 锚点 7
   为"flush / compact / read 三线并发时不 UAF"预留的接口。

2. **写锁保护**：`Add` / `Get` 都先 `Lock()`。本阶段只放一把互斥锁，
   正确性优先；**W3 会改成 Group Commit（多个写请求合并一次 fsync）+ 放开锁**，
   把单锁的串行瓶颈打开。

3. **MVCC 快照读**：`Get` 时传入 `sequence`，只返回 `seq <= snapshot`
   的最新版本。查找键用 `LookupKey` 编码（见 6.3），
   跳表里第一个 user_key 匹配且 `seq <= snapshot` 的项就是答案；
   遇到 `kTypeDeletion` 墓碑则视为不存在。

**记录编码**（`Add` 写入跳表的内容）：
```
| varint(ik_size) | internal_key (user_key + 7B seq + 1B type) | varint(val_size) | value |
```
整条记录从 Arena 分配，key 与 value 都内联在节点里，没有额外堆分配。

> 踩坑：最初 `Get` 误用 `comparator_->CompareUserKey(internal_key, user_key)`，
> 但 `CompareUserKey` 会对**两个参数都调用 `ExtractUserKey`**（要求二者都是内部键）。
> 把裸 `user_key`（如 `"foo"` 3 字节）喂进去会触发 `ExtractUserKey` 的
> 长度断言。修复：直接 `user_comparator()->Compare(ExtractUserKey(internal_key), user_key)`。
> ASAN 没报越界，说明这是逻辑错而非堆损坏 —— 印证了"断言比 sanitizer 更早抓到语义错误"。

### 6.3 `LookupKey` —— 零拷贝查找键

`Get` 时用户只给 `user_key` + `sequence`，但跳表按 `InternalKey` 排序。
`LookupKey` 在栈上把二者拼成一条完整内部键：
```
| varint(ik_size) | user_key | packed(seq, kTypeValue) |
```
- `memtable_key()`：带 varint 长度前缀，用于跳表 `FindGreaterOrEqual`
  （前缀让"第一个 >= 的"比较一次完成，不用反复算长度）；
- `internal_key()`：去掉长度前缀，做精确的 user_key 比较与 sequence 比较；
- `user_key()`：做前缀截断比较。
三档视图指向同一块缓冲，零拷贝。

### 6.4 `WriteBatch` —— 把"一批写"当成一个原子单位

W1 锚点 5 定死：**WAL 以 `WriteBatch` 为记录单位**。
这样要么一批全部恢复、要么全不恢复，是后续加事务（原子性）的地基。

**格式**：
```
| sequence (8B) | count (4B) | [ Put/Delete 记录 ]* |
```
每条记录：`| tag(1B) | varint(key_len) | key | varint(val_len) | value |`。

**两个关键设计**：
- `Handler` 回调接口：`Iterate` 遍历每条记录时通过 `Put`/`Delete` 回调出去，
  `MemTableInserter`（实现 `Handler`）把它插进 MemTable。
  这样 WriteBatch 本身不依赖 MemTable 的具体类型，**解耦**。
- `InsertInto` / `Append`：恢复时把写批里的记录逐个插回 MemTable；
  `Append` 把两个写批拼接成更大的批（Group Commit 用）。

`WriteBatchInternal` 作为友元藏在 `write_batch.h` 里，
提供 `SetSequence` / `Sequence` 等"只有 DB 才该调"的内部操作，
把批量写入的序列号分配逻辑从用户 API 里隔离出来。

### 6.5 `WAL` —— 预写日志：崩溃后绝不丢数据

**物理记录格式**：
```
每个分片： | crc32c(4B, 小端) | length(2B, 小端) | type(1B) | data(length 字节) |
文件按 kBlockSize = 32KB 切块。
```
**为什么要切块 + 分片（FIRST / MIDDLE / LAST）**：
WAL 是纯追加日志。崩溃往往发生在"写到一半"，最后一个块可能只有半个 record。
把文件切成固定块、每条 record 最多填满块内剩余空间，放不下的切成分片，
下一分片从新块开头继续。这样"半条 record"只影响最后一个块，
前面的分片仍能拼回完整 record —— **崩溃恢复只需处理最后一个不完整的块**，复杂度常数级。

**Reader 的重组逻辑**（核心难点）：
`ReadPhysicalRecord` 按块读出 `[header|data]`，`ReadRecord` 用状态机把
`FIRST → MIDDLE* → LAST` 拼回一条逻辑 record。遇到 `kZeroType`（块内填充）跳过，
遇到 `EOF`（最后一个不完整块）正常结束而非报错，遇到 crc 失配通过 `Reporter`
上报并停止。

> 踩坑（W2 最隐蔽的 bug，花了一轮才定位）：
> `ReadPhysicalRecord` 里 `*result = Slice(buffer_.data(), length)` 指向的是
> **头部**，不是数据 —— 因为修复"长度检查顺序"时我把 `remove_prefix(kHeaderSize)`
> 合并成了 `remove_prefix(kHeaderSize + length)`，却忘了 result 应当跳过头部。
> 正确写法（LevelDB 同款）：`Slice(buffer_.data() + kHeaderSize, length)`。
> 这个 bug 表现为"读到的数据是头部字节、crc 校验随机失配"，且失败记录不固定
> （取决于后续数据），典型的"静默数据损坏"，靠 ASAN 都不一定抓得到，
> 最终是标准校验向量 + 大记录往返测试暴露的。

### 6.6 `CRC32C` —— 为什么 WAL 要有校验和，以及"标准值"的坑

**为什么需要**：WAL 是唯一一道"掉电也能恢复"的防线。磁盘/文件系统会悄悄篡改数据
（写到一半掉电的半个块、SSD 静默扇区损坏）。没有校验和，恢复时会把损坏字节
当成合法记录，造成数据错误而非崩溃；有校验和，坏 record 变成一次明确的
`Corruption` 错误，可安全跳过。

**为什么是 CRC32C 而不是简单异或**：单字节校验只能发现奇数个比特翻转，
对"整段被零覆盖"完全无能为力。CRC32C 对突发错误极其敏感，能检出所有
<= 32 位突发错误，且 SSE4.2 有硬件指令 `crc32`。

**算法约定（踩坑重灾区）**：
CRC32C 的标准校验向量是 `"123456789" -> 0xE3069283`，
但这是**反射（LSB-first）算法 + init=0xFFFFFFFF + xorout=0xFFFFFFFF** 的结果。
我第一版写成了 MSB-first（非反射）表，即便加上 init/xorout 也只得到
`0x05440F15`；而 init=0、xorout=0 的"裸"MSB-first 值是 `0xC052A8C8`
—— 两者都不是 `0xE3069283`。

最终采用与 SSE4.2 一致的**反射实现**：
- 查表多项式用反射形式 `0x82F63B78`（即 Castagnoli `0x1EDC6F41` 按位反射）；
- `Extend` 进入寄存器前 `crc ^= 0xFFFFFFFFu`，结束时再 `^= 0xFFFFFFFFu`；
- 这样 `Value("123456789") == 0xE3069283`，且与硬件指令语义对齐，
  将来可无修改地替换为 `_mm_crc32_u8`。

`crc32c_test.cpp` 的 `StandardCheckValue` 就用这个向量守着算法约定，
确保"查表方向 / init / xorout"任一写错都会立刻红。

---

## 七、W3 设计取舍（DB / Group Commit / 无锁读）

W2 把 WriteBatch / WAL / MemTable / SkipList / CRC32C 五个组件做好了，
但彼此独立、没有串成一条"写路径"。W3 的目标就是**第一次把它们接成一个能写、
能读、能持久化、能崩溃恢复的引擎**，并顺手解决写路径上两个最大的瓶颈：
**Group Commit**（把多次 fsync 合并成一次）和**放开 MemTable 读写锁**。

### 7.1 `DB` —— 把组件接成写路径的胶水层

新增 `DB`（接口）+ `DBImpl`（实现），对外只暴露 `Open / Put / Delete / Get / Write`：

- `Open`：建目录 → `Recover`（见 7.5）→ 建立可写 WAL；
- `Write(batch)`：Group Commit 把 batch 追加进 WAL、回放到 MemTable；
- `Put/Delete`：包一层单条记录的 WriteBatch 转发给 `Write`；
- `Get`：直接查 MemTable，走无锁快照读（见 7.4）。

刻意保持精简：**W3 不做 flush / SSTable / Compaction**（那是 W4+）。
MemTable 是唯一的数据归宿，进程退出即丢；真正的持久化靠"每次写都 fsync 的 WAL"，
下次 `Open` 重放即可恢复。这样 W3 聚焦在写路径的两个优化上，不被读路径的
复杂度分心。

### 7.2 Group Commit —— 多个写请求共享一次 fsync

**为什么必须做**：`WritableFile::Sync()` 就是 `fsync`，在机械盘上是**毫秒级**
操作（SSD 也要几十微秒）。如果每次 `Put` 都自己 fsync，写入 QPS 被 fsync 延迟
死死钉住；而实际上同一瞬间到达的成百上千个写，完全可以先在内存里拼成一个大
batch，**整组只 fsync 一次**。fsync 次数从 N 降到 N / 平均组大小，通常是数量级提升。

**实现（leader/follower 模型）**：
```cpp
Status DBImpl::Write(const WriteBatch& my_batch) {
  Writer w{ &my_batch, false };
  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&w);
  // 不是队首 -> 已有 leader 在干活，等它唤醒（leader 会把我的 batch 一起提交）
  while (!w.done && &w != writers_.front()) w.cv.wait(lock);
  if (w.done) return w.status;

  // 我是 leader：把队列里所有等待者合并成一个大 batch
  WriteBatch combined;
  for (Writer* it : writers_)
    if (it->batch && it->batch->Count() > 0)
      WriteBatchInternal::Append(&combined, it->batch);

  SequenceNumber seq = last_sequence_ + 1;
  WriteBatchInternal::SetSequence(&combined, seq);
  Status s = log_->AddRecord(combined.Contents());
  if (s.ok()) s = logfile_->Sync();          // ★ 整组只 fsync 一次
  if (s.ok()) {
    WriteBatchInternal::InsertInto(&combined, mem_.load(acquire));
    last_sequence_.store(seq + combined.Count() - 1, release);  // 先写内存，再发布 seq
  }
  while (!writers_.empty()) {                // 唤醒同组所有 follower
    Writer* ready = writers_.pop_front();
    if (ready != &w) ready->status = s;
    ready->done = true; ready->cv.notify_one();
  }
  return s;
}
```

- `mutex_` 只保护 `writers_` 队列 + 把"WAL 追加 + fsync + 回放"串成**单写者**；
  它**不**保护读路径（见 7.4），所以读不被这次 fsync 阻塞。
- 队首线程成为 **leader**，负责把整组提交完；其余线程是 **follower**，
  在各自 `cv` 上等待，被唤醒时本批已被 leader 写好、结果填在 `status` 里。
- 复用 W2 的 `WriteBatch::Append` 做批拼接——这正是 W2 锚点"WAL 以 WriteBatch
  为记录单位"的价值：合并只是字节串拼接，回放逻辑完全复用。

**正确性不变量**：同一组共享同一个提交结果 `status`；sequence 连续分配，
组内第 i 条记录的 sequence = 组起始 seq + i，回放进 MemTable 后正好是
"要么整组可见、要么整组不可见"（原子性来自 WAL 一次落盘）。

### 7.3 内存序：last_sequence_ 的 release / acquire

`last_sequence_` 是已提交的最大 sequence，读者用它做 MVCC 快照读。关键约束：
**必须先回放进 MemTable，再用 release 发布新的 last_sequence_**。这样读者用
acquire 读到新 `last_sequence_` 时，happens-before 保证它一定能看到对应的
MemTable 内容——不会出现"snapshot 说能看到 seq=100，但内存里 seq=100 还没插完"
的撕裂读。

`mem_` 本身用 `std::atomic<MemTable*>`（acquire 读 / release 写），为 W4 的
flush "换表"留好无锁切换的口子；W3 里它全程稳定，只在 `Open` 时发布一次。

### 7.4 放开 MemTable 读写锁 —— 读完全不被写阻塞

W2 里 `MemTable::Add` 和 `Get` 都取同一把 `mu_`。W3 把 `Get` 的锁**去掉**：
```cpp
Status MemTable::Get(...) const {
  LookupKey lkey(user_key, snapshot);
  SkipList<...>::Iterator iter(&table_);   // 不取 mu_
  ...                                     // 沿 next_ 指针走，全是 atomic acquire 读
}
```
**为什么安全**：跳表从设计上就支持"单写者 + 多无锁读者"——`next_` 指针是
`std::atomic`，写者插入时用 `release` 发布，读者用 `acquire` 读，不会看到撕裂
的指针；节点由 Arena 一次性分配、随 MemTable 整体释放，读者期间通过 `DB::Get`
里的 `Ref/Unref` 保活（W4 引入 flush 换表时，这层引用计数立刻生效，读不会被
正在落盘的旧 MemTable 销毁）。`Add` 仍由 `mu_`（或 DB 写者队列）串行化，保证
"单写者"前提。

效果：快照读与前台写**完全并行**。`db_test.ConcurrentWritesAndReads` 用
8 线程边写边读，TSAN 全程零竞争报告——这是"按需求选最弱同步"的又一次实践：
读路径零互斥，正确性全靠原子指针的 acquire/release。

> 踩坑（W3）：
> 1. 最初 `Get` 一旦取 `mu_`，就会被 leader 那次毫秒级 fsync 阻塞——所谓
>    "放开读写锁"就名存实亡。所以读路径必须彻底不碰 `mutex_`，只靠 `mem_` /
>    `last_sequence_` 的 atomic 完成。
> 2. `last_sequence_` 的发布顺序：必须先 `InsertInto` 再 `store(release)`。
>    反过来（先发布 seq 再回放）会让读者用新 snapshot 去查还没写完的内存，
>    得到错误结果而非崩溃——这类 bug TSAN 抓不到，靠 `db_test` 的最终一致性
>    校验（所有 key 都该读到正确值）兜底。

### 7.5 崩溃恢复：重放 WAL 重建 MemTable

`Recover` 在 `Open` 时：读旧 WAL 的每条 record（本身就是一条 WriteBatch 字节串），
用 `WriteBatch::SetContents` 灌进一个 `WriteBatch` 再 `InsertInto` 回放到 MemTable，
同时用头部 seq 恢复 `last_sequence_`；恢复完建立一条**截断重写**的新 WAL
（内容已进内存，无需保留旧日志）。WAL 的"分片 + crc + 最后一个不完整块当 EOF"
机制（见 W2 6.5）保证即使崩溃发生在写一半，也只丢弃那半个块，前面的 record
全部可恢复。`db_test.RecoverAfterReopen` 覆盖"写 200 条 + 删 1 条 → 销毁 →
重开 → 数据完好（含墓碑）"。

### 7.6 Group Commit 的真实收益：测量与诚实结论

`db_bench` 对比单线程顺序写与多线程并发写。本机（WSL2，WAL 落在 `/tmp`，
fsync 极快）实测并发写只比顺序写略高——**因为 fsync 本身够快，合并的收益被
线程创建 / 调度开销掩盖**。这是必须诚实写进来的结论：

> Group Commit 的 10× 级收益来自"把 N 次毫秒级 fsync 合成 1 次"。在 fsync
> 便宜的环境（tmpfs、部分 SSD、WSL 的虚拟磁盘）上几乎看不出来；在真实机械盘
> 或高 fsync 延迟的云盘上才会显著。收益是**结构性**的，不依赖本机测速数字。

这恰好是面试里比"报一个漂亮数字"更有说服力的点：能说清"优化在哪、为什么本机
测不出来、真实场景会怎样"。

---

## 八、下一步（W4）——写于 W3 结束时，保留作为规划快照

> **这一节是当时（写完 W3）的规划，原样保留。** 实际执行与它有两处出入，见 §九。

写路径已经完整且高效（Group Commit + 无锁读），但 MemTable 是纯内存、进程退出
即丢、且无法支撑大于内存的数据集。W4 进入**读路径与持久化层**：

- **SSTable**：不可变的有序文件，内部切成 data block + index block + bloom block；
- **Block 读取 + Bloom Filter**：把"点查"从"全表扫"降到 O(1) 次 IO；
- **flush**：MemTable 写满后整体落成一个 SSTable（用 W3 的引用计数安全换出）；
- **版本管理（Version / VersionSet / MANIFEST）**：让 flush / 读 / 未来的
  compaction 三线并发时，旧 SSTable 不被正在读的线程销毁（W1 锚点 6/7 在此落地）；
- **读路径统一**：Get 先查 MemTable，未命中再查 SSTable 层，合并出最新可见版本。

之后才是 Compaction、MVCC 多版本回收、以及可选的 Raft 复制。

**与实际执行的出入**：

1. 计划里没有预估到的问题——W4 落地后立刻需要一轮 `fix(W4)`（见 §九.2）。
   读路径的并发与持久化耦合比写路径复杂得多，一次性写对的概率很低。
2. 计划把 Compaction 排在 W5 之后，实际是 **W6**——中间插了 W5 迭代器。
   原因是 Compaction 需要"跨文件的归并迭代器"，这个能力必须先有。

---

## 九、W4 读路径与持久化

### 9.1 三个真实缺陷（都不是"写错了"，是"想漏了"）

**① MANIFEST 没存 `last_sequence` —— 症状是"重开后整库读不到"**

flush 的顺序是"SSTable 落盘 → `LogAndApply` 登记 → 删旧 WAL"。执行到第三步时，
最后一个 WAL 已经是空文件，重开时重放它得到 `max_seq = 0`，于是快照点回退到 0。
所有已 flush 的数据 `seq >= 1`，在快照 0 下一条都不可见——**整库读不到，
且不报任何错**。

修法看着简单（`VersionEdit` 加一个 `sequence` 字段），但踩了一个自己挖的坑：
第一版在 `LogAndApply` 里写 `edit->sequence = last_sequence_`，结果把调用方
（`DBImpl`）填好的真实值**覆盖成了 0**——`VersionSet` 自己的 `last_sequence_`
只在 `Recover` 时被填充，运行期并不跟踪写入进度。修 bug 时引入的新 bug，
而且症状与原 bug 一模一样。

> 教训：修"数据丢失"类问题时，**新增的赋值语句要特别检查是不是覆盖了别人的值**。
> 这类 bug 不会立刻暴露，它的表现是"修复无效"。

**② `VersionSet` 必须对 `current_` 持所有权引用**

`current()` 的写法是"load 指针 → 循环 `TryRef`"。而 `TryRef()` 在 `refs_ == 0`
时返回 false——`refs_ == 0` 恰恰意味着"没有引用、即将被 delete"。于是无限自旋。

这个 while 循环**在原理上无法自愈**：它假设"TryRef 失败 = 对象正在销毁，重新
load 即可"，但对象一旦 `delete`，重试 load 也救不回已经悬垂的指针。

修法：让 `VersionSet` 持有初始 `Version` 的一个所有权引用（构造时 `Ref()`，
换版本时对新版本 `Ref()` 再 `Unref()` 旧的），保证读者在锁内一定能 Ref 成功。

> 教训：RCU 的正确性前提不是"引用计数本身"，而是"**存在一个永不归零的锚点
> 引用**"。没有锚点，`TryRef` 就是在给已释放内存投票。

**③ `BlockHandle::DecodeFrom` 传值 = 索引块定位错位**

`Footer` 里连续存了 `meta_index_handle` 和 `index_handle`，解码时要依次取出。
原实现签名是 `DecodeFrom(const Slice&)`——按值传参，内部只移动了副本，
外层的 `rest` 纹丝不动。于是第二个 handle 从**第一个 handle 的字节**开始解，
拿到的"索引块"其实是元数据块。

改成 `DecodeFrom(Slice*)` 原地消耗。同类问题：前端还出现过"内存代码被写入工具
报告成功但实际没落盘"的情况，只能靠 `grep` 复核发现——**任何"成功"返回都要
用独立手段验证**。

### 9.2 fix(W4)：六项审查修复

代码审查（自审 + 人工走查）发现的，按影响排序：

| # | 问题 | 后果 |
|---|---|---|
| 1 | `TableBuilder::Flush/Finish` 丢弃 `WriteBlock` 返回值 | data/filter/index 写失败时 `Finish()` 仍返回 OK → 登记进 MANIFEST → 删旧 WAL → **数据永久丢失且无报错** |
| 2 | `ReadBlock` 的 `offset + size + 5` 可整数溢出 | 损坏 footer 里的巨大值回绕成小数，边界检查被绕过 |
| 3 | `Get` 把 `Corruption` 降级成 `NotFound` | 磁盘损坏被静默伪装成"key 不存在"，排障时看不到任何痕迹 |
| 4 | `Options` 默认 `filter_policy = NewBloomFilterPolicy(10)` | 每构造一个 `Options` 漏一个策略对象（ASAN 报 11 处） |
| 5 | `CompactMemTable` 失败时泄漏孤儿 WAL / 忽略 `Close()` | 清理不完整 |
| 6 | `versions_->current()` 未判空 | 防御缺口 |

第 1 条与第 3 条是同一类问题：**"静默"**。项目此后把"绝不静默丢数据"写成
第一条设计目标（见 `architecture.md` §一），它也成了此后每次代码审查的一级判据。

第 4 条的修法值得单独说：`Options` 是可随意值拷贝的轻量配置对象，不该持有
任何堆所有权。改为指向**进程级静态单例**（借用、不拥有）——`Options` 的拷贝与
析构都不再产生分配。

---

## 十、W5 迭代器

`DBIterator` 把 MemTable 与全部 SSTable 作为子迭代器**归并**，同 `user_key` 的
多版本按 `internal_key` 顺序集中出现，逐版本做可见性判断后只吐出一个。

**关键决策 1：`NewIterator` 不自动 `SeekToFirst`**

初始位置为"无效"，由调用方显式选择 `SeekToFirst` / `SeekToLast` / `Seek`。
职责清晰，也避免给 W8 的反向遍历埋语义歧义。原来的实现调了 `SeekToFirst`
然后 `return`——后面还有一段永远不会执行的死代码（`it->SeekToFirst(); return it;`），
是加这个包装层时留下的残留。

**关键决策 2：`FindShortestSeparator` 只能压缩 `user_key` 部分**

`internal_key = user_key + 8 字节后缀`。若直接对整个 internal key 做前缀压缩，
可能产出长度 < 8 的**非法内部键**，后续解码即崩。正确做法是只压缩 `user_key`，
再重新拼回 `InternalKey(tmp, kMaxSequenceNumber, kTypeValue)`。

**测试踩坑：违反有序输入契约**

构造 SSTable 时我按写入顺序塞入 `key00000..key00099, mv, del`，但 `"del" < "key00000" < "mv"`
——**字典序不等于写入序**。`TableBuilder` 要求严格有序输入，乱序会产出无法正确
seek 的表。测试当时表现为"有的 key 读不到"，很容易误判为 block 索引有问题。

修法是把测试改成用 `icmp` 排序后再建表，让排序器承担这个契约，而不是靠手写顺序。

---

## 十一、W6 Compaction

Leveled 策略：L0 由 flush 产出、键范围互相重叠（查询要整层扫），更高层由
compaction 产出、键范围互不重叠（可二分定位）。

**关键决策：`VersionEdit` 里"新增输出文件"与"删除源文件"必须在同一条记录里**

这条记录落盘之前崩溃 → 旧文件仍在、新文件只是孤儿，重启后被清理；
落盘之后崩溃 → 两边都已登记，数据完整。**绝不能分两次提交**——中间状态会让
源文件和输出文件都"存在但未登记"，或都不存在。

**TableCache 必须有引用计数**

compaction `delete` 旧文件时，若有并发读者正拿着它的 `Table*`，那个指针立刻悬垂。
W6 给缓存条目加了 `refs`（当前读者数）与 `evicted`（已标记淘汰、待引用归零后
真正释放）两个字段。

**留到 W8 才修的缺陷：`next_is_final` 算了却没用**

`CompactMemTable` 里算出 `next_is_final` 用来决定"沉到最底层就丢弃墓碑"，
但这个变量**从未被传给归并器**——所有层的压缩都在丢弃墓碑。后果：L1→L2 压掉了
墓碑，而该 key 可能还有更老的版本躺在 L3，于是**旧值复活**。

同一处还有第二个问题：压缩硬编码 `kMaxSequenceNumber` 作为归并快照，等于把
"比当前所有快照都新"的版本也一起丢掉。W8 引入 Snapshot 句柄后，这里才改成
"以最早活跃快照为下界"。

---

## 十二、W7 Compaction 后台化

`MaybeScheduleCompaction` 只做"判断 + 投任务"，真正的 IO 在后台线程——写路径
不因压缩而阻塞。

**析构时如何确认"没有任务在跑"**：`active_compactions_` 归零，再往队列投一个
barrier 任务并等它执行完。Env 的队列是 FIFO，barrier 一旦跑到就说明前面的任务
都执行完毕。

**为什么 `WaitForBackgroundCompaction` 忙等而不用条件变量**

条件变量需要在 `~DBImpl` 里销毁，而后台线程可能恰好在 `notify_all`——
TSAN 会把这种"逻辑上已唤醒"的并发判为 data race。忙等只依赖析构方独占的那把锁，
不引入需要额外销毁的同步原语。代价是析构期间有轮询开销，但那段路径本就不该
是热路径。

**W7 同时让一个原本极窄的竞态变成常态**

写路径此前是单线程的（`mutex_` 保护一切），读路径的引用计数窗口极窄。
把 compaction 挪到后台线程后，flush（写线程）与 compaction（后台线程）
**两条路径都会调 `LogAndApply` 换 Version**——W10 审查发现的第一个 UAF 正是
这个变化的后果。

---

## 十三、W8：P1 收口

四项工作，详见 `architecture.md` §五的决策表，这里只记两个"想清楚才发现"的点。

**反向遍历不能照搬正向的 MVCC 规则**

归并序里同 `user_key` 的版本按 `seq` **降序**排列。正向走（internal_key 升序）
是"先新后老"，取第一个 `seq <= snapshot` 的即可。反向走就是"**先老后新**"——
照搬会读到**最老**的版本。

而且 key 序列完全正确，只有 value 错——这类缺陷最难被察觉。改法是反向时整键
扫过，取 `seq <= snapshot` 的最大值。

**前缀压缩让"回退一步"无法直接解析**

`Block::Iter::Prev` 的核心难题：entry 头只存"与**前**一条共享多少字节"，
要还原前一条的完整 key，得知道它**之前**那条的完整 key——依赖链只能正向解开。

解法是从最近的重启点**重新正向扫一小段**、取其中最后一条。这个代价能被重启点
间隔（默认 16）限制在常数级，**如果没有重启点，反向一步就要从块首重扫**，
退化成 O(n)。换句话说，重启点机制的第二个作用是"让反向遍历可行"，而不只是
"加速正向查找"。

**另一个坑：测试变空过**

W8 新增的缓存 LRU 测试最初设 `max_table_cache_bytes = 1`，断言 `entries <= 1`。
它在 4MB 默认缓冲下 200 条写入全部留在 MemTable，压根不产生 SSTable，
`entries` 恒为 0，断言因"0 <= 1"恒真而**看起来是绿的**。

我怀疑后做了变异验证（禁用 LRU 淘汰）——测试依然通过。才意识到它什么都没验证。
修正：加小 `write_buffer_size` 强制产生 SSTable，并补 `ASSERT_GT(NumTableFiles(), 1)`
作为前置断言。

> 教训：**一个恒真的断言比没有测试更危险**——它提供了虚假的信心。
> 写完断言要问"什么情况下它会失败"，答不上来就说明它是空过。

---

## 十四、W9 可靠性：库级排他锁

`Env::LockFile` 早在 W1 就实现好了（`flock` + `LOCK_NB`），但**从未被调用**。
两个进程同开一个目录会各自维护独立的 MemTable 与 VersionSet，并发写同一个
MANIFEST、互删对方的 SSTable——全程无报错，一路静默把数据写坏。

选 `flock` 而不是"创建一个 LOCK 文件当标志"：进程退出（含 `kill -9`）时内核
自动释放不留死锁；`LOCK_NB` 立刻报错而非卡住；加锁与"是否已被占用"是同一个
原子操作，不存在"检查-再打开"的竞态窗口。

**抢锁的时机**：放在 `CreateDirIfMissing` 之后、VersionSet 恢复**之前**。
反过来会让第二个进程先完成 MANIFEST 重放、进入可写状态才抢锁失败，届时它已经
建好 VersionSet，必须完整回滚才能不留痕迹。先抢锁则失败得干净。

**已知局限**：`flock` 锁的是"打开文件描述"而非进程，同进程内两次 `Open` 拦不住。
这是 flock 的既定语义，真实风险来自多进程——所以锁的测试必须用两个真实进程。

---

## 十五、W9 崩溃测试：两个只有真踩过才知道的坑

用 `fork + exec` 制造真实的"进程被杀"，验证**已返回 OK 的写入**在崩溃后不丢。

**坑 1：只用 `fork` 会死锁，而且测试会静默地什么都没验证**

第一版只用 `fork`，compaction 用例直接挂死。根因：`Env` 的后台线程池是**进程级
单例**，而 `fork` 只复制调用线程。子进程里线程池对象还在、执行线程却不存在：
- 子进程调 `Schedule()` 只是把任务塞进队列，没人消费；
- 析构时 `WaitForBackgroundCompaction()` 往队列投 barrier 任务并等待 → 永远等不到 → 死锁。

更隐蔽的是：compaction 根本不跑，"输出已落盘"那个崩溃点**永远打不到**，
测试会变成"什么都没验证"却看起来是绿的。

加上 `exec` 后子进程是全新进程映像：Env 单例重建、后台线程重启、锁状态归零。

**坑 2：`setenv` 不能用于父子传参**

它会调 `malloc`，而 fork 后子进程继承了其他线程的 malloc 锁状态。改用 `argv`
（fork 前就备好字符串）+ `execv`，全程只读访问。

---

## 十六、W10 代码审查修复

详见 `code-review.md`。这里只记两件"修 bug 时才浮现"的东西。

**① 修 UAF 时引入了内存泄漏，功能测试全绿**

把 `Ref()` 移进锁内（修 UAF）之后，LeakSanitizer 报出 56 处泄漏：
`NewIterator` 里 `Ref()` 一次，`MemTableIteratorAdapter` 构造时**又** `Ref()`
一次，析构只 `Unref()` 一次——**两次 Ref、一次 Unref**，永久泄漏一个引用。

这个错误功能测试完全正常（数据一字不差），只有进程退出时 LeakSanitizer 才能发现。
如果只跑功能测试就提交，等于**把一个内存破坏 bug 换成了一个内存泄漏 bug**。

改法：适配器语义从"构造时新增引用"改为"接管调用方的引用"，并在类注释写明约定。

> 教训：三套配置里 ASAN 不是可选项而是**必需品**。功能测试全绿不等于正确。

**② MANIFEST 损坏测试暴露的"巧合保护"**

`CorruptedManifestMiddleIsRejected` 在去掉状态检查后**仍然失败**——但错误是
`current log file missing` 而非 `Corruption`。

因为 `log_number` 回退到了某个**已被删除**的 WAL，次生报错才让 `Open` 没能成功。
**危险正在于此**：只要回退到的 `log_number` 恰好指向一个**仍存在**的 WAL，
`Open` 就会成功，而新 WAL 与已登记的 SSTable 已被双向删除——数据消失且无报错。

也就是说这个缺陷看起来"有其他保护兜着"，实际上那只是巧合。任何据此认为
"影响有限"的判断都是错的。

> 教训：评估缺陷影响时，**偶然会出错的代码不是防护**。要问"它在什么情况下
> 不出错"，而不是"它出错时我看到了什么症状"。

---

## 十七、下一步

已完成 W1~W10。剩余方向与优先级见 `README.md` 的「后续计划」。

一句话总结这条时间线：**每个阶段最大的收获都不是"实现了功能"，而是"发现了
自己上一阶段想漏了什么"**。W4 发现了持久化协议里 sequence 的缺口，W6 发现了
墓碑下沉没接上，W8 发现了测试可以空过，W10 发现了"修 UAF 会引入泄漏"。

