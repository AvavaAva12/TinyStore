#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tinystore/env.h"

namespace tinystore {

namespace {

// ---------------------------------------------------------------------------
// 顺序读文件
// ---------------------------------------------------------------------------
class PosixSequentialFile final : public SequentialFile {
public:
    PosixSequentialFile(std::string filename, int fd)
        : fd_(fd), filename_(std::move(filename)) {}

    ~PosixSequentialFile() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Status Read(size_t n, Slice* result, char* scratch) override {
        for (;;) {
            const ssize_t r = ::read(fd_, scratch, n);
            if (r < 0) {
                // EINTR 表示系统调用被信号打断，这不是真正的错误，重试即可。
                // 在带信号处理的生产环境里，漏掉这个分支会导致
                // 偶发的、极难复现的 IOError。
                if (errno == EINTR) {
                    continue;
                }
                return IOErrorFromErrno(filename_, errno);
            }
            // r == 0 表示 EOF，此时 Slice 长度为 0，由调用方判断。
            // 注意不要把它当成错误 —— 读到文件尾是正常情况。
            *result = Slice(scratch, static_cast<size_t>(r));
            return Status::OK();
        }
    }

    Status Skip(uint64_t n) override {
        if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) ==
            static_cast<off_t>(-1)) {
            return IOErrorFromErrno(filename_, errno);
        }
        return Status::OK();
    }

private:
    const int fd_;
    const std::string filename_;
};

// ---------------------------------------------------------------------------
// 随机读文件
// ---------------------------------------------------------------------------
class PosixRandomAccessFile final : public RandomAccessFile {
public:
    PosixRandomAccessFile(std::string filename, int fd)
        : fd_(fd), filename_(std::move(filename)) {}

    ~PosixRandomAccessFile() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Status Read(uint64_t offset, size_t n, Slice* result,
                char* scratch) const override {
        ssize_t r = -1;
        do {
            // pread 是"定位 + 读取"的原子操作，且**不修改文件偏移量**。
            // 这一点至关重要：SSTable 会被多个查询线程并发读取，
            // 如果用 lseek + read，线程之间会互相破坏偏移量，
            // 必须额外加锁；用 pread 则天然无竞争，无需任何同步。
            r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
        } while (r < 0 && errno == EINTR);

        if (r < 0) {
            return IOErrorFromErrno(filename_, errno);
        }
        *result = Slice(scratch, static_cast<size_t>(r));
        return Status::OK();
    }

private:
    const int fd_;
    const std::string filename_;
};

// ---------------------------------------------------------------------------
// 顺序写文件
// ---------------------------------------------------------------------------
class PosixWritableFile final : public WritableFile {
public:
    PosixWritableFile(std::string filename, int fd)
        : fd_(fd), filename_(std::move(filename)) {}

    ~PosixWritableFile() override {
        if (fd_ >= 0) {
            // 析构时只关闭，不做 fsync —— 是否落盘由调用方通过 Sync() 决定。
            // 这是一个刻意的取舍：析构函数不应该有"意外"的昂贵副作用。
            ::close(fd_);
        }
    }

    Status Append(const Slice& data) override {
        const char* src = data.data();
        size_t left = data.size();

        // 必须循环：write() 不保证一次写完所有请求的字节。
        // 对于普通文件通常一次就够，但在以下情况会"短写"（short write）：
        //   * 磁盘满（ENOSPC 之前先写了一部分）
        //   * 写入被信号中断
        //   * 某些网络文件系统（NFS）
        // 直接假设 write 一定写完，是很多自制存储引擎的经典 bug。
        while (left > 0) {
            const ssize_t done = ::write(fd_, src, left);
            if (done < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return IOErrorFromErrno(filename_, errno);
            }
            left -= static_cast<size_t>(done);
            src += done;
        }
        return Status::OK();
    }

    Status Flush() override {
        // POSIX 的 write() 返回后，数据已经在内核页缓存里了，
        // 不需要（也没有）额外的用户态缓冲区要刷。
        // 因此这里天然就是 no-op。
        //
        // 对比：如果未来改用带用户态缓冲的实现（比如为了减少 syscall 次数
        // 而自己攒 buffer），Flush() 就必须把攒的数据 write 出去。
        // 接口先留着，实现可以随策略演进。
        return Status::OK();
    }

    Status Sync() override {
        // fsync vs fdatasync 的取舍：
        //   fsync      刷数据页 + 所有元数据（文件大小、mtime、atime 等）
        //   fdatasync  只刷数据页 + 读取所必需的元数据（跳过 mtime 等）
        //
        // 理论上 fdatasync 更快（少写一次元数据），
        // 但对追加写的 WAL 来说，文件大小本身就是关键元数据，必须落盘，
        // 两者实际差异很小。这里选 fsync 以保证语义最保守。
        // W3 会用 benchmark 实测两者差距，再决定是否优化。
        if (::fsync(fd_) != 0) {
            return IOErrorFromErrno(filename_, errno);
        }
        return Status::OK();
    }

    Status Close() override {
        if (fd_ < 0) {
            return Status::OK();  // 已关闭，幂等
        }
        const int r = ::close(fd_);
        fd_ = -1;  // 先置 -1，避免析构时重复 close（double close 是严重 bug：
                   // fd 号可能被别的线程复用，close 掉别人的 fd）
        if (r != 0) {
            return IOErrorFromErrno(filename_, errno);
        }
        return Status::OK();
    }

private:
    int fd_;
    const std::string filename_;
};

// ---------------------------------------------------------------------------
// 文件锁
// ---------------------------------------------------------------------------
class PosixFileLock final : public FileLock {
public:
    PosixFileLock(std::string filename, int fd)
        : fd_(fd), filename_(std::move(filename)) {}

    ~PosixFileLock() override {
        if (fd_ >= 0) {
            // 关闭 fd 会自动释放该进程的 flock，
            // 这里只是兜底（正常情况下 UnlockFile 已经解过了）。
            ::close(fd_);
        }
    }

    int fd() const { return fd_; }
    const std::string& filename() const { return filename_; }

private:
    const int fd_;
    const std::string filename_;
};

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
class PosixLogger final : public Logger {
public:
    explicit PosixLogger(FILE* file) : file_(file) {}

    ~PosixLogger() override {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    void Log(const std::string& message) override {
        // 后台 Compaction 线程也会写日志，必须加锁保护 FILE*。
        // （glibc 的 FILE 操作本身有内部锁保证单次调用原子，
        //   但这里需要保证"消息 + 换行"这两次写入不被其他线程插队。）
        std::lock_guard<std::mutex> lk(mu_);
        std::fwrite(message.data(), 1, message.size(), file_);
        std::fwrite("\n", 1, 1, file_);
        std::fflush(file_);  // 立即刷出，保证崩溃时日志不丢
    }

private:
    std::mutex mu_;
    FILE* file_;
};

// ---------------------------------------------------------------------------
// POSIX 环境实现
//
// 内部维护一个简单的后台线程池：目前固定 1 个线程，
// 后续 Compaction 压力上来后可以按 CPU 核数扩展。
// ---------------------------------------------------------------------------
class PosixEnv final : public Env {
public:
    PosixEnv() {
        // 启动后台线程。它负责执行所有 Schedule() 进来的任务
        // （未来主要是 MemTable flush 和 Compaction）。
        bg_threads_.emplace_back([this] { BGThread(); });
    }

    ~PosixEnv() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            shutting_down_ = true;
        }
        bgsignal_.notify_all();

        // 等后台线程把队列里剩余的任务做完再退出。
        // 这一步很重要：如果直接退出，正在 flush 的 MemTable 会丢失，
        // 而这些数据已经向客户端返回过"写入成功"了。
        for (std::thread& t : bg_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // --- 文件 ---
    Status NewSequentialFile(
        const std::string& fname,
        std::unique_ptr<SequentialFile>* result) override {
        const int fd = ::open(fname.c_str(), O_RDONLY);
        if (fd < 0) {
            *result = nullptr;
            return IOErrorFromErrno(fname, errno);
        }
        *result = std::make_unique<PosixSequentialFile>(fname, fd);
        return Status::OK();
    }

    Status NewRandomAccessFile(
        const std::string& fname,
        std::unique_ptr<RandomAccessFile>* result) override {
        const int fd = ::open(fname.c_str(), O_RDONLY);
        if (fd < 0) {
            *result = nullptr;
            return IOErrorFromErrno(fname, errno);
        }
        *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
        return Status::OK();
    }

    Status NewWritableFile(
        const std::string& fname,
        std::unique_ptr<WritableFile>* result) override {
        // O_TRUNC：文件已存在则清空。SSTable 和 WAL 都是全新写入，
        // 不需要保留旧内容。
        const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                              0644);
        if (fd < 0) {
            *result = nullptr;
            return IOErrorFromErrno(fname, errno);
        }
        *result = std::make_unique<PosixWritableFile>(fname, fd);
        return Status::OK();
    }

    Status NewAppendableFile(
        const std::string& fname,
        std::unique_ptr<WritableFile>* result) override {
        // O_APPEND 的关键性质：每次 write 都原子地定位到文件末尾。
        // 这样即使有多个线程/进程同时追加，也不会互相覆盖。
        // MANIFEST 依赖这个特性做"只追加"的版本记录。
        const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                              0644);
        if (fd < 0) {
            *result = nullptr;
            return IOErrorFromErrno(fname, errno);
        }
        *result = std::make_unique<PosixWritableFile>(fname, fd);
        return Status::OK();
    }

    bool FileExists(const std::string& fname) override {
        return ::access(fname.c_str(), F_OK) == 0;
    }

    Status GetChildren(const std::string& dir,
                       std::vector<std::string>* result) override {
        result->clear();
        DIR* d = ::opendir(dir.c_str());
        if (d == nullptr) {
            return IOErrorFromErrno(dir, errno);
        }

        // glibc 的 readdir() 是线程安全的（返回的 dirent 由 DIR 自己持有），
        // 但 POSIX 标准并不保证这一点。为了可移植性，调用方不应
        // 对同一个 DIR* 并发调用 readdir。
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(d)) != nullptr) {
            result->emplace_back(entry->d_name);
        }
        ::closedir(d);
        return Status::OK();
    }

    Status DeleteFile(const std::string& fname) override {
        if (::unlink(fname.c_str()) != 0) {
            return IOErrorFromErrno(fname, errno);
        }
        return Status::OK();
    }

    Status CreateDir(const std::string& dirname) override {
        if (::mkdir(dirname.c_str(), 0755) != 0) {
            return IOErrorFromErrno(dirname, errno);
        }
        return Status::OK();
    }

    Status CreateDirIfMissing(const std::string& dirname) override {
        if (::mkdir(dirname.c_str(), 0755) == 0) {
            return Status::OK();
        }
        if (errno == EEXIST) {
            // 已存在时，必须确认它是目录而不是同名的普通文件，
            // 否则后续在该路径下创建文件会出现莫名其妙的错误。
            struct stat sbuf{};
            if (::stat(dirname.c_str(), &sbuf) == 0 && S_ISDIR(sbuf.st_mode)) {
                return Status::OK();
            }
            return Status::IOError(dirname, "exists but is not a directory");
        }
        return IOErrorFromErrno(dirname, errno);
    }

    Status DeleteDir(const std::string& dirname) override {
        if (::rmdir(dirname.c_str()) != 0) {
            return IOErrorFromErrno(dirname, errno);
        }
        return Status::OK();
    }

    Status GetFileSize(const std::string& fname, uint64_t* size) override {
        struct stat sbuf{};
        if (::stat(fname.c_str(), &sbuf) != 0) {
            *size = 0;
            return IOErrorFromErrno(fname, errno);
        }
        *size = static_cast<uint64_t>(sbuf.st_size);
        return Status::OK();
    }

    Status RenameFile(const std::string& src,
                      const std::string& dst) override {
        // rename 在同一文件系统内是原子的。
        // 这是"先写临时文件再改名"这一模式（write-ahead 思想的常用手法）
        // 的正确性基础：外界要么看到旧文件，要么看到新文件，
        // 绝不会看到一个写了一半的文件。
        if (::rename(src.c_str(), dst.c_str()) != 0) {
            return IOErrorFromErrno(src + " -> " + dst, errno);
        }
        return Status::OK();
    }

    // --- 锁 ---
    Status LockFile(const std::string& fname,
                    std::unique_ptr<FileLock>* lock) override {
        *lock = nullptr;
        const int fd = ::open(fname.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            return IOErrorFromErrno(fname, errno);
        }

        // flock 是"建议性锁"（advisory lock），且归属于**进程**：
        //   * 进程退出（包括 kill -9）时内核自动释放，不会留下永久死锁，
        //     这比用"创建锁文件"这种土办法可靠得多；
        //   * LOCK_NB 表示拿不到锁立刻返回错误，而不是阻塞等待，
        //     这样可以对用户报出清晰的错误信息。
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            return Status::IOError(fname,
                                   "lock already held by another process");
        }

        *lock = std::make_unique<PosixFileLock>(fname, fd);
        return Status::OK();
    }

    Status UnlockFile(FileLock* lock) override {
        // 用 dynamic_cast 做类型检查：接口只承诺了 FileLock 基类，
        // 防御性地校验可以避免传错实现时产生未定义行为。
        auto* posix_lock = dynamic_cast<PosixFileLock*>(lock);
        if (posix_lock == nullptr) {
            return Status::InvalidArgument("lock is not a PosixFileLock");
        }
        if (::flock(posix_lock->fd(), LOCK_UN) != 0) {
            return IOErrorFromErrno(posix_lock->filename(), errno);
        }
        return Status::OK();
    }

    // --- 线程 ---
    void Schedule(std::function<void()> fn) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!shutting_down_) {
                queue_.push_back(std::move(fn));
                // notify_one 即可：每次只唤醒一个线程取一个任务。
                // 用 notify_all 会造成"惊群"（thundering herd），
                // 多个线程被唤醒却只有一个能拿到任务，白白浪费 CPU。
                bgsignal_.notify_one();
                return;
            }
        }
        // 走到这里说明 Env 正在关闭，不会再有线程来取任务。
        // 此时必须在当前线程同步执行，否则任务会被静默丢弃 ——
        // 如果这个任务是"flush MemTable"，就意味着已经向客户端
        // 确认过的写入丢失了，是严重的数据一致性问题。
        fn();
    }

    void StartThread(std::function<void()> fn) override {
        std::thread t(std::move(fn));
        t.detach();  // 独立生命周期，不需要 join
    }

    // --- 测试与辅助 ---
    Status GetTestDirectory(std::string* path) override {
        // 固定路径而非带 pid：本项目的测试在单进程内串行执行，
        // 固定路径便于失败后直接 ls 查看残留文件排查问题。
        *path = "/tmp/tinystore-test";
        return CreateDirIfMissing(*path);
    }

    Status NewLogger(const std::string& fname,
                     std::unique_ptr<Logger>* result) override {
        FILE* f = ::fopen(fname.c_str(), "w");
        if (f == nullptr) {
            *result = nullptr;
            return IOErrorFromErrno(fname, errno);
        }
        *result = std::make_unique<PosixLogger>(f);
        return Status::OK();
    }

    uint64_t NowMicros() override {
        // 用 steady_clock 而不是 system_clock：
        //   steady_clock 是单调时钟，不受 NTP 校时 / 手动改时间影响，
        //   用它计算"时间间隔"语义正确；
        //   system_clock 可能回退，用两次采样相减会得到负数。
        // 这里返回的是"相对于本进程启动时刻"的微秒数。
        static const auto kEpoch = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - kEpoch)
                .count());
    }

    void SleepForMicroseconds(int micros) override {
        std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }

private:
    void BGThread() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);

                // 条件变量的正确用法：必须用带谓词的 wait，
                // 并处理"虚假唤醒"（spurious wakeup）。
                // 用 if 而非 while 判断条件，被虚假唤醒后会带着空任务继续执行。
                bgsignal_.wait(lk, [this] {
                    return shutting_down_ || !queue_.empty();
                });

                if (queue_.empty()) {
                    // 队列已排空且收到关闭信号 -> 可以安全退出
                    if (shutting_down_) {
                        return;
                    }
                    continue;
                }

                task = std::move(queue_.front());
                queue_.pop_front();
            }

            // 在**锁外**执行任务：
            // 1. 避免在任务执行期间阻塞其他线程的 Schedule 调用；
            // 2. 任务里可能再次调用 Schedule（例如 Compaction 分阶段执行），
            //    如果持锁会造成死锁。
            task();
        }
    }

    std::mutex mu_;
    std::condition_variable bgsignal_;
    bool shutting_down_ = false;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> bg_threads_;
};

}  // namespace

Env* Env::Default() {
    // 刻意不释放（intentional leak）。两个原因：
    //
    // 1. 避免静态析构顺序问题（static destruction order fiasco）：
    //    其它翻译单元里的静态对象在析构时可能仍会调用 Env::Default()。
    //    如果用函数内静态对象，它在退出时会被析构，
    //    之后再用就是未定义行为。
    //
    // 2. 避免退出时 join 后台线程可能引发的挂起：
    //    如果析构时后台任务正在执行某个长时间操作，
    //    进程可能在 join 处卡住。
    //
    // 进程退出后操作系统会回收全部资源，这里的"泄漏"没有任何实际影响。
    // 对于必须在退出时清理的资源（如 POSIX 文件锁），
    // 内核会在 fd 关闭时自动释放，因此也是安全的。
    static PosixEnv* default_env = new PosixEnv();
    return default_env;
}

}  // namespace tinystore
