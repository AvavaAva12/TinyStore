#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tinystore/slice.h"
#include "tinystore/status.h"

namespace tinystore {

// ===========================================================================
// Env —— 操作系统抽象层
// ===========================================================================
//
// 【为什么必须有这一层】
// 如果代码里到处直接调用 open/read/write/fsync/std::thread，会付出三个代价：
//
//   1. 无法做故障注入测试。存储引擎最核心的正确性保证是"掉电后数据不丢
//      且可恢复"，这种场景在普通测试里根本无法构造。有了 Env 抽象，
//      就能写一个 TestEnv 在第 N 次 Write 时随机返回 IOError，
//      验证恢复逻辑在各种截断/损坏下都正确。这是本项目的关键加分项。
//
//   2. 无法做确定性测试。Compaction 的触发依赖时间（NowMicros）。
//      把时钟也抽象进来后，测试可以用 MockEnv 手动推进时间，
//      让原本依赖 sleep 的测试变成确定性的、毫秒级完成的单元测试。
//
//   3. 未来难以扩展。做 Raft 时，"写一份日志"这个动作要从"写本地文件"
//      变成"写本地文件 + 复制给多数派节点"。如果 WAL 只依赖 Env 接口，
//      这个改造就只是换一个 Env 实现，而不是重写 WAL。
//
// 【为什么用 out 参数（指针）返回，而不是返回智能指针】
// C++17 可以用 std::variant 做 StatusOr<T>，C++23 有 std::expected。
// 这里沿用 out 参数的风格，理由是：
//   * 与 Status 的设计保持一致（错误通过返回值，成功通过 out 参数）；
//   * 避免了 StatusOr<T> 在非平凡类型上的额外移动构造开销；
//   * 调用方写法统一：
//         std::unique_ptr<WritableFile> file;
//         Status s = env->NewWritableFile(path, &file);
//         if (!s.ok()) return s;
// ===========================================================================

// ---------------------------------------------------------------------------
// 文件锁
//
// 用途：防止两个进程同时打开同一个数据库目录。
// 两个进程各自维护自己的 MemTable 和 VersionSet，同时写同一个 MANIFEST
// 会直接导致数据损坏，必须在最外层用文件锁挡住。
// ---------------------------------------------------------------------------
class FileLock {
public:
    FileLock() = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    virtual ~FileLock() = default;
};

// ---------------------------------------------------------------------------
// 日志接口
// ---------------------------------------------------------------------------
class Logger {
public:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    virtual ~Logger() = default;

    // 传 std::string 而非格式化串：格式化由调用方完成，
    // 保持接口简单，也避免子类各自实现一套 printf 解析。
    virtual void Log(const std::string& message) = 0;
};

// ---------------------------------------------------------------------------
// 顺序读文件
//
// 用于读取 WAL 和 MANIFEST —— 这两种文件都是从头到尾扫一遍。
// ---------------------------------------------------------------------------
class SequentialFile {
public:
    SequentialFile() = default;
    SequentialFile(const SequentialFile&) = delete;
    SequentialFile& operator=(const SequentialFile&) = delete;
    virtual ~SequentialFile() = default;

    // 从当前位置读取至多 n 字节。
    //
    // 【为什么要有 scratch 参数】
    // 返回的 Slice 不拥有数据（见 slice.h），所以缓冲区必须由调用方提供。
    // 这样调用方可以复用同一块 buffer 连续读取，避免每读一次就分配一次内存。
    // 在恢复 WAL 时（可能上百万条记录）这个优化非常可观。
    //
    // 返回值语义：
    //   * *result 的长度 < n 表示已读到文件尾（EOF），这是正常情况，不是错误；
    //   * 真正的 I/O 失败通过返回的 Status 表达。
    virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

    // 跳过 n 字节（等价于 lseek(SEEK_CUR)），用于跳过不关心的记录
    virtual Status Skip(uint64_t n) = 0;
};

// ---------------------------------------------------------------------------
// 随机读文件
//
// 用于读取 SSTable：先读 footer 定位索引，再随机跳转到指定 offset 读数据块。
// ---------------------------------------------------------------------------
class RandomAccessFile {
public:
    RandomAccessFile() = default;
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    virtual ~RandomAccessFile() = default;

    // 【为什么是 pread 而不是 lseek + read】
    // pread 是原子的定位+读取，**不修改文件的当前偏移量**。
    // SSTable 会被多个读线程并发访问，如果用 lseek+read，
    // 两个线程会互相破坏对方的偏移量，必须额外加锁。
    // 用 pread 则天然线程安全，无需任何锁。这是设计上的一个重要取舍。
    virtual Status Read(uint64_t offset, size_t n, Slice* result,
                        char* scratch) const = 0;
};

// ---------------------------------------------------------------------------
// 顺序写文件
//
// 用于写 WAL 和 SSTable。
// ---------------------------------------------------------------------------
class WritableFile {
public:
    WritableFile() = default;
    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;
    virtual ~WritableFile() = default;

    virtual Status Append(const Slice& data) = 0;

    // 把用户态缓冲区刷到内核页缓存（page cache）。
    // 【重要】这**不保证**数据落盘：进程崩溃不会丢，但机器掉电会丢。
    virtual Status Flush() = 0;

    // 真正把数据刷到物理磁盘（fsync）。
    // 【性能提示】fsync 的耗时在机械盘上可达毫秒级，是写入路径的主要开销。
    // W3 会用 Group Commit（多个写请求合并一次 fsync）把这个开销摊薄，
    // 通常能带来一个数量级的 QPS 提升。
    virtual Status Sync() = 0;

    // 关闭文件。调用后对象不应再使用。
    virtual Status Close() = 0;
};

// ---------------------------------------------------------------------------
// Env 主接口
// ---------------------------------------------------------------------------
class Env {
public:
    Env() = default;
    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    virtual ~Env() = default;

    // 返回进程的默认 Env（PosixEnv 单例）
    static Env* Default();

    // --- 文件 ---
    virtual Status NewSequentialFile(
        const std::string& fname, std::unique_ptr<SequentialFile>* result) = 0;

    virtual Status NewRandomAccessFile(
        const std::string& fname, std::unique_ptr<RandomAccessFile>* result) = 0;

    // 创建新文件（已存在则截断）
    virtual Status NewWritableFile(
        const std::string& fname, std::unique_ptr<WritableFile>* result) = 0;

    // 以追加模式打开（已存在则从尾部写）。
    // MANIFEST 需要这个语义：每次版本变更都追加一条记录，
    // 而不是重写整个文件 —— 这样即使写到一半崩溃，
    // 已有的历史记录也不会被破坏。
    virtual Status NewAppendableFile(
        const std::string& fname, std::unique_ptr<WritableFile>* result) = 0;

    virtual bool FileExists(const std::string& fname) = 0;

    virtual Status GetChildren(const std::string& dir,
                               std::vector<std::string>* result) = 0;

    virtual Status DeleteFile(const std::string& fname) = 0;

    virtual Status CreateDir(const std::string& dirname) = 0;

    virtual Status CreateDirIfMissing(const std::string& dirname) = 0;

    virtual Status DeleteDir(const std::string& dirname) = 0;

    virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;

    // 原子重命名。用于"先写临时文件，再原子改名"的模式：
    // SSTable 生成过程中如果崩溃，只会留下一个不完整的临时文件，
    // 不会被误认为是一个完整的 SSTable。
    virtual Status RenameFile(const std::string& src,
                              const std::string& dst) = 0;

    // --- 锁 ---
    virtual Status LockFile(const std::string& fname,
                            std::unique_ptr<FileLock>* lock) = 0;
    virtual Status UnlockFile(FileLock* lock) = 0;

    // --- 线程 ---
    // 把一个任务扔到后台线程执行。用于 Compaction、MemTable flush 等
    // 不应该阻塞前台写入的耗时操作。
    virtual void Schedule(std::function<void()> fn) = 0;

    // 创建一个独立的新线程（不复用线程池），用于长生命周期的服务线程
    virtual void StartThread(std::function<void()> fn) = 0;

    // --- 测试与辅助 ---
    virtual Status GetTestDirectory(std::string* path) = 0;

    virtual Status NewLogger(const std::string& fname,
                             std::unique_ptr<Logger>* result) = 0;

    // 当前时间（微秒）。做成虚函数是为了测试时可注入假时钟。
    virtual uint64_t NowMicros() = 0;

    virtual void SleepForMicroseconds(int micros) = 0;
};

// ---------------------------------------------------------------------------
// EnvWrapper —— 默认把所有调用转发给另一个 Env
//
// 用途：子类只需重写关心的方法，其余保持默认行为。
// 这是实现 TestEnv（故障注入）和 MockEnv（假时钟）的基础设施，
// 也是装饰器模式的又一应用。
//
// 典型用法：
//     class FaultInjectionEnv : public EnvWrapper {
//       Status NewWritableFile(...) override {
//         if (ShouldFail()) return Status::IOError("injected fault");
//         return EnvWrapper::NewWritableFile(fname, result);
//       }
//     };
// ---------------------------------------------------------------------------
class EnvWrapper : public Env {
public:
    explicit EnvWrapper(Env* target) : target_(target) {}
    ~EnvWrapper() override = default;

    Env* target() const { return target_; }

    Status NewSequentialFile(
        const std::string& fname,
        std::unique_ptr<SequentialFile>* result) override {
        return target_->NewSequentialFile(fname, result);
    }

    Status NewRandomAccessFile(
        const std::string& fname,
        std::unique_ptr<RandomAccessFile>* result) override {
        return target_->NewRandomAccessFile(fname, result);
    }

    Status NewWritableFile(
        const std::string& fname,
        std::unique_ptr<WritableFile>* result) override {
        return target_->NewWritableFile(fname, result);
    }

    Status NewAppendableFile(
        const std::string& fname,
        std::unique_ptr<WritableFile>* result) override {
        return target_->NewAppendableFile(fname, result);
    }

    bool FileExists(const std::string& fname) override {
        return target_->FileExists(fname);
    }

    Status GetChildren(const std::string& dir,
                       std::vector<std::string>* result) override {
        return target_->GetChildren(dir, result);
    }

    Status DeleteFile(const std::string& fname) override {
        return target_->DeleteFile(fname);
    }

    Status CreateDir(const std::string& dirname) override {
        return target_->CreateDir(dirname);
    }

    Status CreateDirIfMissing(const std::string& dirname) override {
        return target_->CreateDirIfMissing(dirname);
    }

    Status DeleteDir(const std::string& dirname) override {
        return target_->DeleteDir(dirname);
    }

    Status GetFileSize(const std::string& fname, uint64_t* size) override {
        return target_->GetFileSize(fname, size);
    }

    Status RenameFile(const std::string& src,
                      const std::string& dst) override {
        return target_->RenameFile(src, dst);
    }

    Status LockFile(const std::string& fname,
                    std::unique_ptr<FileLock>* lock) override {
        return target_->LockFile(fname, lock);
    }

    Status UnlockFile(FileLock* lock) override {
        return target_->UnlockFile(lock);
    }

    void Schedule(std::function<void()> fn) override {
        target_->Schedule(std::move(fn));
    }

    void StartThread(std::function<void()> fn) override {
        target_->StartThread(std::move(fn));
    }

    Status GetTestDirectory(std::string* path) override {
        return target_->GetTestDirectory(path);
    }

    Status NewLogger(const std::string& fname,
                     std::unique_ptr<Logger>* result) override {
        return target_->NewLogger(fname, result);
    }

    uint64_t NowMicros() override { return target_->NowMicros(); }

    void SleepForMicroseconds(int micros) override {
        target_->SleepForMicroseconds(micros);
    }

private:
    Env* target_;
};

}  // namespace tinystore
