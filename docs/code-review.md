# 代码审查报告（第一轮）

> 审查日期：2026-10-09　审查范围：`src/`（16 个实现文件）+ `include/tinystore/`（22 个头文件），逐文件读完。`tests/`、`benchmarks/` 未在本轮审查范围内。
>
> 方法：编译器严格警告（36 处）+ 全量人工走查。每条结论交付前都构造了反例尝试推翻（证伪门禁），未能推翻的才保留。
>
> **本报告只做诊断，不含修复。** 修复需另行确认后进行。

## 摘要

| 级别 | 条数 | 说明 |
|---|---|---|
| 致命 | 0 | — |
| 严重 | 3 | 两处 use-after-free、一处"损坏被当正常" |
| 警告 | 9 | 健壮性缺口、错误处理不一致、API 契约不符 |
| 建议 | 3 | 死代码、注释失真 |

**整体评估**：架构清晰、注释密度高且解释"为什么"，注释与实现的一致性远高于同类项目。但**并发路径存在两处真实 UAF**，且都发生在 W7 把 compaction 挪到后台之后才成为常态。

最值得注意的是 3 条问题**都与"静默"有关**：损坏被当作正常结束、错误被降级为不存在、注释声称的安全属性实际不成立。项目注释里反复强调"绝不能静默丢数据"，而这三条恰好违反了这个原则。

---

## 严重问题

### S1. `CompactLevel` 选源文件时留下悬垂指针 ✅已验证

**位置**：`src/db.cpp:499-507`

```cpp
std::vector<const FileMetaData*> inputs;      // 裸指针
{
  Version* v = versions_->current();          // 持引用
  for (const auto* f : v->FilesAtLevel(level)) inputs.push_back(f);
  v->Unref();                                 // ← 引用归零，Version 可能被 delete
}
if (level > 0 && inputs.size() > 1) {
  const FileMetaData* smallest =
      *std::min_element(inputs.begin(), inputs.end(), ...);   // ← 读已释放内存
```

**问题**：`inputs` 保存的是 `Version::files()` 里元素的**裸指针**，而 `Unref()` 之后 `Version` 可能被回收（`VersionSet` 换版本时 `old->Unref()` 会 `delete`）。第 512 行的 `min_element`、第 525-529 行的 `input_numbers.push_back` 与 `RangesOverlap` 都在 `Unref()` 之后解引用这些指针。

**触发条件**：`Unref()` 后到解引用之间发生 `LogAndApply`（并发 flush 或 compaction）。**这不是边缘场景**——W7 之后 compaction 在后台线程、flush 在写路径线程，两条路径都会换 Version。

**后果**：读到随机的 `FileMetaData`，导致 ①选错 compaction 输入文件；②`RangesOverlap` 判断错乱，多余文件参与归并或该参与的不参与；③把不该合并的文件写进 `VersionEdit`，**丢数据或读不到 key**，且无任何报错。

**修复建议**：改为按值持有（拷贝 `FileMetaData` 本身，它只含 3 个字符串+数字，拷贝成本远低于一次 compaction 的 IO）：

```cpp
std::vector<FileMetaData> inputs;
{
  Version* v = versions_->current();
  if (v == nullptr) return Status::OK();
  if (v->files().empty()) { v->Unref(); return Status::OK(); }
  for (const auto& f : v->FilesAtLevel(level)) inputs.push_back(f);
  v->Unref();
}
```

**反向推演**：若 `VersionSet` 对 `current_` 的所有权引用保证旧 Version 在整个 `CompactLevel` 期间存活，则本条不成立。已确认不成立——`LogAndApply` 中 `old->Unref()` 会在归零时 `delete old`。

---

### S2. `NewIterator` 的 MemTable 引用在锁外建立 ✅已验证

**位置**：`src/db.cpp:1033-1040`

```cpp
MemTable* m = nullptr;
{
  std::lock_guard<std::mutex> lk(mem_mutex_);
  m = mem_.load(std::memory_order_relaxed);   // 只 load，没有 Ref
}                                                // ← 锁在这里就放了
if (m != nullptr) {
  it->AddChild(std::make_unique<MemTableIteratorAdapter>(m));  // 锁外才 Ref
}
```

**问题**：`Ref()` 实际发生在 `MemTableIteratorAdapter` 构造函数里，位于**锁外**。而同一文件里 `Get()` 的写法是正确的（`src/db.cpp:941-949`，锁内完成 load + Ref），两条读路径的写法不一致。

**触发条件**：`load` 到 `m` 之后、适配器构造之前，flush 线程执行 `mem_.store(new_mem); imm->Unref()`。若这是最后一个引用，`imm` 被 `delete`，适配器随后对已释放内存调用 `Ref()`。

**后果**：use-after-free，且后续整个迭代器生命周期都在踩悬垂内存。

**注释与实现不符**（构成"假注释"）：

> 注释称"适配器在构造时 Ref、析构时 Unref，所以即使迭代途中发生 flush 把这个 MemTable 换出去，它也一定存活到迭代器销毁"

机制描述正确，但**时机错了**——Ref 不在锁内完成，锁无法保证"load 到 m"与"m 存活"之间没有窗口。

**为什么 TSAN / ASAN 没抓到**：需要 `NewIterator` 与 flush 精确交叠。db_test 里迭代测试与并发测试是分开的，没有交叉覆盖。**这是测试覆盖的盲区，不是实现正确。**

**修复建议**：与 `Get` 对齐，锁内完成 Ref（适配器构造保持不变，它会在析构时 Unref）：

```cpp
MemTable* m = nullptr;
{
  std::lock_guard<std::mutex> lk(mem_mutex_);
  m = mem_.load(std::memory_order_relaxed);
  if (m != nullptr) m->Ref();
}
if (m != nullptr) {
  it->AddChild(std::make_unique<MemTableIteratorAdapter>(m));
} else {
  // m 为 nullptr 时不需要 Ref，适配器也不会构造
}
```

---

### S3. `log::Reader` 无法区分"读完"与"损坏"，MANIFEST/WAL 损坏被静默吞掉 ✅已验证

**位置**：`src/version_set.cpp:213-237`（MANIFEST）、`src/db.cpp:205-218`（WAL）

```cpp
log::Reader reader(file.get(), nullptr, true);   // reporter = nullptr
while (reader.ReadRecord(&record, &scratch)) {   // false = EOF 或损坏，无法区分
  ...
}
// 循环结束后当作"正常读完"继续
```

**问题**：`ReadRecord` 用 `bool` 返回，EOF、CRC 不匹配、坏记录长度、未知记录类型**全部返回 `false`**。而 `reporter` 传了 `nullptr`，意味着损坏连日志都不会有。调用方无法区分二者。

**后果分两种，必须区分对待**：

- **尾部截断**（写 MANIFEST 途中掉电）：可恢复。`LogAndApply` 把 `new_files` + `log_number` + `sequence` 放在**同一条记录**里（`db.cpp:299-317`），所以丢掉最后一条不会产生"中间态"。该次 flush 未生效 → SSTable 被当孤儿清理 → 旧 WAL 仍在 → 数据从 WAL 重放恢复。**但错误被静默吞掉，排障时看不到任何线索。**

- **中段损坏**（位翻转、硬件故障）：**真实丢数据且无任何报错**。重放到损坏点即停止 → 后续若干次已提交的 flush 对应的 `.ldb` 被 `db.cpp:174-183` 当孤儿删除；同时 `log_number` 回退到更早的值，使那些数据所在的**新 WAL 也被当作孤儿删除**（`db.cpp:180`）。数据在两个方向上同时消失。

**修复建议**：`log::Reader` 增加 `Status status()`，把损坏与 EOF 区分开（`log_reader.h:38-69` 现在的文档声称损坏会交给 Reporter，实际上 Reporter 只用于日志，不影响返回值）。最小改动是让 `ReadRecord` 在损坏时返回一个可区分的状态，或增加一个 `bool ReadRecord(..., Status* err)` 重载；调用方在循环结束后检查并上报 `Corruption`。

---

## 警告

### W1. `Block` 构造对重启点数组无下界校验 ✅已验证

**位置**：`src/block.cpp:85-91`

```cpp
num_restarts_ = DecodeFixed32(data_ + size_ - 4);
restarts_ptr_ = data_ + size_ - 4 - 4 * static_cast<size_t>(num_restarts_);
entries_end_ = static_cast<uint32_t>(restarts_ptr_ - data_);
```

缺少 `4 * num_restarts_ + 4 <= size_` 的校验。若 `num_restarts_` 是损坏数据里的垃圾值，`restarts_ptr_` 指到 `data_` **之前**，指针差为负，转 `uint32_t` 回绕成巨大值 → `entries_end_` 失效 → `ParseEntry` 的边界检查形同虚设 → 堆越界读。

**触发条件**：块内容损坏且 CRC 恰好通过（4 字节碰撞，概率 1/2³²）或校验被绕过。概率低，但这是一道廉价的后防线。

**修复建议**：构造时校验，不满足则令 `num_restarts_ = 0`（所有迭代器入口都会先判 `num_restarts_ == 0`，天然安全）。

---

### W2. `metaindex` 块损坏被吞成"打开成功" ✅已验证

**位置**：`src/table.cpp:263-284`

```cpp
s = t->ReadBlock(f.meta_index_handle, &meta_contents);
if (s.ok()) { ... }        // ← s 失败时什么都不做
*table = t;
return Status::OK();       // ← 无论 s 如何都返回 OK
```

**后果**：损坏的 SSTable 被当作正常表放进 `TableCache`。因 metaindex 里只有 filter 条目，表现为"该表没有过滤器"——读放大上升但**不影响正确性**。真正的问题是损坏现场丢失：排障时看到的是"读变慢了"，而不是"这个文件坏了"。

**修复建议**：区分处理——metaindex 损坏返回 `Corruption`；metaindex 内容不含 filter 条目（正常情况）才算 OK。

---

### W3. `LevelCapacity` 在 `max_level_bytes_multiplier == 0` 时整数除零 ✅已验证

**位置**：`src/db.cpp:401`

```cpp
if (cap > UINT64_MAX / options_.max_level_bytes_multiplier) {   // multiplier=0 → 除零
```

`max_level_bytes_multiplier` 是 `Options` 公开可配字段（默认 10），用户显式设为 0 即触发 UB。任意 `level >= 1` 的容量判断都会走到这里。

**修复建议**：在 `DB::Open` 校验配置（`multiplier >= 1`、`write_buffer_size > 0` 等），或在此处加 `if (options_.max_level_bytes_multiplier == 0) return cap;` 兜底。**建议前者**——配置校验应集中在入口，而不是每个使用点各自防御。

---

### W4. `Get` 丢弃 `MemTable::Get` 返回的 Corruption ✅已验证

**位置**：`src/db.cpp:968-970`

```cpp
Status s = m->Get(key, snapshot, value, &found);
if (found) { m->Unref(); return s; }
m->Unref();     // ← found==false 时 s 被完全丢弃
```

`MemTable::Get` 在无法解析 internal key 时返回 `Corruption` 且 `*found` 保持 false（`memtable.cpp:73-75`）。此时该 Status 被丢弃，继续查 SSTable 并最终可能返回 `NotFound`。

**这与同一文件 `db.cpp:998-1006` 的注释直接矛盾**——那段注释专门论证"非 NotFound 的错误必须上报，不能静默伪装成 key 不存在"，而 MemTable 侧恰好漏了。

**修复建议**：与 SSTable 侧统一处理——`if (!s.ok() && !s.IsNotFound()) { m->Unref(); return s; }`。

---

### W5. `WriteBatchInternal::InsertInto` 忽略 `Iterate` 返回值 ✅已验证

**位置**：`src/write_batch.cpp:108`

```cpp
batch->Iterate(&ins);      // 返回值丢弃
```

**后果**：`db.cpp:213-216` 随后仍按头部 `count` 推进 `max_seq = first + n - 1`，但实际一条都没插入 → `last_sequence_` 被推进到一个从未写入的区间，**sequence 出现空洞**，且这批数据静默丢失。

**触发条件**：WAL 重放时读到一条损坏记录，或调用方误用 `SetContents`。

**修复建议**：`Iterate` 失败时向上传播，调用方据此放弃该记录。

---

### W6. `create_if_missing` 是从未被读取的选项 ✅已验证

`grep create_if_missing src/` 在实现中**零命中**（`db.cpp:119` 只读了 `error_if_exists`）。而 `DB::Open` 无条件调用 `CreateDirIfMissing`。

**后果**：语义与选项相反。用默认 `Options{}`（`create_if_missing=false`）打开不存在的目录会**静默创建**，用户以为会得到 `NotFound`。

**修复建议**：在 `Open` 中实现该语义，或删除该选项。前者更符合用户预期。

---

### W7. 层号 off-by-one：L(max_num_levels-1) 压缩后落到永不参与压缩的层 ✅已验证

**位置**：`src/db.cpp:427`（循环 `lvl < max_num_levels`）、`src/db.cpp:494`（`next_level = level + 1`）

`max_num_levels = 7`，`PickCompactionLevel` 可返回 6，`CompactLevel(6)` 输出文件 `level = 7`。下一轮循环不检查 level 7 → **该批文件永久不再被压缩**。

**触发条件**：`LevelCapacity(6) = 8MB × 10⁵ = 8TB` 被突破。实践中不会发生，属理论缺陷。

**附带说明**：`next_is_final` 在 `next_level == 7` 时为 true，墓碑在 L7 被丢弃——这一点**侥幸正确**（L7 之下确无更老的层）。

**修复建议**：`CompactLevel` 入口拒绝 `level + 1 >= max_num_levels`，或让 `PickCompactionLevel` 返回值限定为 `0..max_num_levels-2`。

---

### W8. WAL 与 compaction 输出均未检查 `Close()`，与自身注释冲突 ✅已验证

**位置**：`src/db.cpp:355`（WAL）、`src/db.cpp:608`（compaction 输出）；对照 `src/db.cpp:297-305` 的注释

`CompactMemTable` 用一大段注释论证"必须检查 `Close`，否则可能把不完整文件登记进 MANIFEST"，然后自己在 WAL 滚动和 compaction 输出处都没检查。

**后果**：compaction 输出文件的 `Close` 失败（刷缓冲阶段出错）会被忽略，该文件随后被登记进 MANIFEST——**数据不完整但被当成有效文件**。

**修复建议**：两处补上 `Close()` 检查。

---

### W9. 13 处 `env_->DeleteFile` 全部忽略返回值 ✅已验证（grep 计数）

**影响**：unlink 失败（权限、只读挂载）导致孤儿文件堆积。本次运行期间占磁盘，重启时会被清理。**风险最低的一条**，列出仅为完整性——其中 `db.cpp:363`（删旧 WAL）失败的后果相对特殊：数据本身安全，但现象无提示。

---

## 建议

### S-A. 死代码与死成员（无功能影响，仅维护噪音）

| 位置 | 内容 |
|---|---|
| `version_set.cpp:295` | `VersionSet::AddLiveFiles` 全工程无调用点，与 `Recover` 的出参机制重复 |
| `table.cpp:387` | `Table::ApproximateOffsetOf` 无调用者（compaction 用 `FileSize()` 粗略切分） |
| `block.cpp:148,263,309` | `Block::Iter::restart_index_` 只写不读（`Prev` 自己重算） |
| `memtable.h:145` | `MemTable::Iterator::mem_` 构造后从未读取 |
| `arena.h:73-84` | `Arena::MemoryUsage()` 无调用者，**其注释仍称"会被后台 flush 线程读取以判断是否落盘"，与实际（已改用 `data_size_`）矛盾** |
| `log_reader.cpp:20-28` | `SkipToInitialBlock()` 恒返回 true 且从不真的跳过；`initial_offset` 传入非 0 会被忽略，而 `log_reader.h:41` 的文档声称支持 |

### S-B. 注释与代码不一致

| 位置 | 问题 |
|---|---|
| `src/db.cpp:110-113` | 注释称"unique_ptr 的析构会调 UnlockFile（flock LOCK_UN + close）"，但 `~DBImpl` **从未调用** `UnlockFile`，`PosixFileLock` 的析构（`env_posix.cpp:203-209`）只做 `::close(fd)`。结论（内核会释放）仍成立，但描述的机制不存在 |
| `src/db.cpp:1031-1032` | 见 S2，注释声称的安全属性实际不成立 |
| `src/db.cpp:297-305` vs `355`/`608` | 见 W8，注释的结论未被遵守 |
| `include/tinystore/skiplist.h:33-36` | 称"搜索路径上的读取用 relaxed 即可"，实际 `FindGreaterOrEqual` 调的是 acquire 版 `Next()`。仅描述不准，无正确性问题 |

### S-C. 严格警告 36 处

绝大多数是 `-Wsign-conversion`（`size_t`/`int` 混算），集中在 `db_iterator.h`（子迭代器下标）、`db.cpp`（`files()` 下标）、`log_writer.cpp`。另有 3 处 `-Wuseless-cast`。

这些**目前都不是 bug**，但按项目自己的"严格警告（提交前跑一次）"约定，属于该清理的欠账。其中 `db.cpp` 用 `int` 下标遍历 `vector` 的写法（`for (int i = static_cast<int>(v->files().size()) - 1; i >= 0; --i)`）值得改成 `size_t` 逆序迭代，顺带消除 S1 的引用计数疑虑。

---

## 待复核项（本轮未亲自验证，不建议直接采信）

以下由自动化走查提出、但我未逐条读代码确认，可信度低于上述条目：

1. `block.cpp:133` `ParseEntry` 中 `shared <= prev_key.size()` 仅 `assert` 保护，NDEBUG 下无检查——若成立，`Prev()` 的回扫路径在损坏数据上会越界读。
2. `version_set.cpp:124` `new_files.back().level = static_cast<int>(lvl)`，损坏的 varint64 可能使 level 变负。
3. `status.cpp:89-113` `ToString` 的 switch 无 default，`code()` 越界时 `std::string(nullptr)` 是 UB。
4. `env_posix.cpp:470` `Schedule` 在 `shutting_down_` 时同步执行任务，若该任务取 `compaction_mu_` 而调用者已持有，会自死锁。因 `Env::Default()` 是故意泄漏的单例，实际可达性存疑。

---

## 修复优先级建议

| 顺序 | 条目 | 理由 |
|---|---|---|
| 1 | **S1 + S2**（两处 UAF） | 内存破坏，且触发路径是常态而非边缘 |
| 2 | **S3**（损坏被吞） | 与项目"绝不静默丢数据"的核心原则直接冲突 |
| 3 | **W4 + W5** | 两处都是"错误被降级"，修复成本极低、收益明确 |
| 4 | W3 + W6 | 配置与 API 契约校验，防用户误配 |
| 5 | W1 + W2 | 损坏数据路径的后防线 |
| 6 | W7 + W8 + W9 | 健壮性补齐 |
| 7 | S-A + S-B + S-C | 清理，不影响正确性 |

**测试缺口**（审查中发现，需要补）：现有测试中，迭代测试与并发测试是**分开**的，没有交叉覆盖——这正是 S1/S2 长期未被发现的原因。建议增加"迭代器存活期间并发 flush/compaction"的用例。