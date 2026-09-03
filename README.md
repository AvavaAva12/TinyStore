# TinyStore

一个从零实现的 **LSM-Tree 存储引擎**，用于系统性学习 C++ 系统级编程。

目标不是"做一个能用的数据库"，而是**把内存、并发、文件 I/O、性能工程这四类
硬骨头逐个啃一遍**，并且每个知识点都能被追问三层。

> 设计决策与踩坑记录见 [`docs/design-notes.md`](docs/design-notes.md) ——
> 那份文档是本项目最重要的资产。

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
├── cmake/                 # 编译选项与 Sanitizer 配置
└── docs/design-notes.md   # 设计笔记（面试素材）
```

---

## 路线图

| 周 | 主题 | 状态 |
|---|---|---|
| **W1** | 工程地基 + `Slice` / `Status` / `Coding` / `Comparator` / `Arena` / `InternalKey` / `Env` | ✅ 完成 |
| W2 | `SkipList` + `MemTable` | ⬜ |
| W3 | WAL + Group Commit + 故障注入 | ⬜ |
| W4 | Block / SSTable 格式 | ⬜ |
| W5 | 读路径：TableCache + 迭代器 + BloomFilter | ⬜ |
| W6 | Flush + Version / VersionSet / MANIFEST | ⬜ |
| W7 | Compaction（Leveled 策略 + 后台线程） | ⬜ |
| W8 | 并发攻坚：锁粒度演进 + 无锁队列 + TSAN | ⬜ |
| W9 | MVCC 快照读 + WriteBatch 事务 | ⬜ |
| W10 | epoll Reactor + RESP 协议 | ⬜ |

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
