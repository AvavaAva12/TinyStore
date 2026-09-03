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
| 单元测试 | `ctest --test-dir build` | ✅ 7/7 通过 |
| 内存安全（ASAN + Debug assert） | `ctest --test-dir build-asan` | ✅ 7/7 通过 |
| 并发安全（TSAN） | `ctest --test-dir build-tsan` | ⚠️ WSL2 下 TSAN 无法运行，见下 |
| 编译警告 | `-Wall -Wextra -Wpedantic -Wshadow ...` | ✅ 零警告 |

Debug 构建下所有 `assert` 均生效，意味着 Arena 对齐、InternalKey 长度、
后缀解析等不变式都经过了运行时校验。

### TSAN 在 WSL2 下的限制与解法

直接运行 TSAN 构建会报：
```
FATAL: ThreadSanitizer: unexpected memory mapping
```
原因是 TSAN 要求进程地址空间落在特定范围内，与 WSL2 内核的
地址随机化（`randomize_va_space=2`）冲突。

**已验证可用的解法**（无需改系统配置，仅对单个进程关闭 ASLR）：
```bash
setarch x86_64 -R ./build-tsan/bin/env_test
```
因此可以把它包进 CMake：让 `add_test` 的命令变成
`setarch -R <exe>`。在 W8 并发攻坚阶段统一接入。

其它备选：
```bash
sudo sysctl -w vm.mmap_rnd_bits=28       # WSL 重启后失效
valgrind --tool=helgrind ./build/bin/env_test   # 慢但一定能跑
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

## 六、下一步（W2）

- `SkipList`：手写，节点内存内联布局（next 指针数组与 key 放同一块内存，缓存友好）
- `MemTable`：基于 SkipList + Arena，所有节点从 Arena 分配
- `LookupKey`：利用 InternalKey 编码实现零拷贝的查找键
