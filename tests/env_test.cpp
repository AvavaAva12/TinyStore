#include "tinystore/env.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

// 断言 Status 为 OK，失败时打印可读的错误信息
#define ASSERT_OK(expr)                                        \
    do {                                                       \
        const Status _status = (expr);                         \
        ASSERT_TRUE(_status.ok()) << _status.ToString();       \
    } while (0)

std::string TestDir() {
    std::string dir;
    const Status s = Env::Default()->GetTestDirectory(&dir);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return dir;
}

std::string TestPath(const std::string& name) { return TestDir() + "/" + name; }

// ---------------------------------------------------------------------------
// 文件读写
// ---------------------------------------------------------------------------

TEST(EnvTest, WriteThenReadSequentially) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_sequential.txt");

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("hello world")));
        ASSERT_OK(file->Sync());
        ASSERT_OK(file->Close());
    }

    std::unique_ptr<SequentialFile> file;
    ASSERT_OK(env->NewSequentialFile(path, &file));

    char scratch[64];
    Slice result;

    ASSERT_OK(file->Read(5, &result, scratch));
    EXPECT_EQ(Slice("hello"), result);

    ASSERT_OK(file->Read(6, &result, scratch));
    EXPECT_EQ(Slice(" world"), result);

    // 越过文件尾：返回 OK，但 Slice 长度为 0。
    // EOF 是正常情况，不是错误 —— 这个语义对 WAL 恢复至关重要。
    ASSERT_OK(file->Read(10, &result, scratch));
    EXPECT_EQ(0u, result.size());

    env->DeleteFile(path);
}

TEST(EnvTest, SequentialReadCanSkipBytes) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_skip.txt");

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("0123456789")));
        ASSERT_OK(file->Close());
    }

    std::unique_ptr<SequentialFile> file;
    ASSERT_OK(env->NewSequentialFile(path, &file));

    ASSERT_OK(file->Skip(4));

    char scratch[16];
    Slice result;
    ASSERT_OK(file->Read(3, &result, scratch));
    EXPECT_EQ(Slice("456"), result);

    env->DeleteFile(path);
}

TEST(EnvTest, RandomAccessReadAtOffset) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_random.txt");

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("0123456789ABCDEF")));
        ASSERT_OK(file->Close());
    }

    std::unique_ptr<RandomAccessFile> file;
    ASSERT_OK(env->NewRandomAccessFile(path, &file));

    char scratch[32];
    Slice result;

    ASSERT_OK(file->Read(4, 3, &result, scratch));
    EXPECT_EQ(Slice("456"), result);

    // 关键性质：pread 不移动文件偏移量。
    // 重复读同一 offset 应得到相同结果（若用 lseek+read 就会出错）。
    ASSERT_OK(file->Read(0, 3, &result, scratch));
    EXPECT_EQ(Slice("012"), result);
    ASSERT_OK(file->Read(4, 3, &result, scratch));
    EXPECT_EQ(Slice("456"), result);

    // 读取范围跨越文件尾：返回实际可读的字节数
    ASSERT_OK(file->Read(14, 10, &result, scratch));
    EXPECT_EQ(Slice("EF"), result);

    // 完全超出文件尾
    ASSERT_OK(file->Read(100, 10, &result, scratch));
    EXPECT_EQ(0u, result.size());

    env->DeleteFile(path);
}

TEST(EnvTest, AppendableFilePreservesExistingContent) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_append.txt");

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("first")));
        ASSERT_OK(file->Close());
    }
    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewAppendableFile(path, &file));
        ASSERT_OK(file->Append(Slice("second")));
        ASSERT_OK(file->Close());
    }

    std::unique_ptr<SequentialFile> file;
    ASSERT_OK(env->NewSequentialFile(path, &file));
    char scratch[32];
    Slice result;
    ASSERT_OK(file->Read(32, &result, scratch));
    EXPECT_EQ(Slice("firstsecond"), result);

    env->DeleteFile(path);
}

TEST(EnvTest, WritableFileTruncatesExistingContent) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_truncate.txt");

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("this will be gone")));
        ASSERT_OK(file->Close());
    }
    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("short")));
        ASSERT_OK(file->Close());
    }

    uint64_t size = 0;
    ASSERT_OK(env->GetFileSize(path, &size));
    EXPECT_EQ(5u, size);

    env->DeleteFile(path);
}

// ---------------------------------------------------------------------------
// 文件系统操作
// ---------------------------------------------------------------------------

TEST(EnvTest, FileExistsAndGetFileSize) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_size.txt");

    EXPECT_FALSE(env->FileExists(path));

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(path, &file));
        ASSERT_OK(file->Append(Slice("12345")));
        ASSERT_OK(file->Close());
    }

    EXPECT_TRUE(env->FileExists(path));

    uint64_t size = 0;
    ASSERT_OK(env->GetFileSize(path, &size));
    EXPECT_EQ(5u, size);

    ASSERT_OK(env->DeleteFile(path));
    EXPECT_FALSE(env->FileExists(path));
}

TEST(EnvTest, ReadingMissingFileReturnsErrorNotCrash) {
    Env* env = Env::Default();
    const std::string path = TestPath("definitely_missing_file");

    std::unique_ptr<SequentialFile> file;
    const Status s = env->NewSequentialFile(path, &file);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsIOError());
    EXPECT_EQ(nullptr, file);

    // 错误信息里应包含文件名，便于排查
    EXPECT_NE(std::string::npos, s.ToString().find(path));
}

TEST(EnvTest, CreateDirIfMissingIsIdempotent) {
    Env* env = Env::Default();
    const std::string dir = TestPath("env_subdir");

    ASSERT_OK(env->CreateDirIfMissing(dir));
    ASSERT_OK(env->CreateDirIfMissing(dir));  // 重复调用不应报错

    EXPECT_TRUE(env->FileExists(dir));

    ASSERT_OK(env->DeleteDir(dir));
    EXPECT_FALSE(env->FileExists(dir));
}

TEST(EnvTest, GetChildrenListsCreatedFiles) {
    Env* env = Env::Default();
    const std::string dir = TestPath("env_children");

    ASSERT_OK(env->CreateDirIfMissing(dir));

    for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(dir + "/" + name, &file));
        ASSERT_OK(file->Append(Slice("x")));
        ASSERT_OK(file->Close());
    }

    std::vector<std::string> children;
    ASSERT_OK(env->GetChildren(dir, &children));

    EXPECT_NE(children.end(),
              std::find(children.begin(), children.end(), "a.txt"));
    EXPECT_NE(children.end(),
              std::find(children.begin(), children.end(), "b.txt"));
    EXPECT_NE(children.end(),
              std::find(children.begin(), children.end(), "c.txt"));

    for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
        ASSERT_OK(env->DeleteFile(dir + "/" + name));
    }
    ASSERT_OK(env->DeleteDir(dir));
}

TEST(EnvTest, RenameFileMovesContent) {
    Env* env = Env::Default();
    const std::string src = TestPath("env_rename_src.txt");
    const std::string dst = TestPath("env_rename_dst.txt");

    {
        std::unique_ptr<WritableFile> file;
        ASSERT_OK(env->NewWritableFile(src, &file));
        ASSERT_OK(file->Append(Slice("payload")));
        ASSERT_OK(file->Close());
    }

    ASSERT_OK(env->RenameFile(src, dst));

    EXPECT_FALSE(env->FileExists(src));
    EXPECT_TRUE(env->FileExists(dst));

    ASSERT_OK(env->DeleteFile(dst));
}

// ---------------------------------------------------------------------------
// 文件锁
// ---------------------------------------------------------------------------

TEST(EnvTest, FileLockIsExclusiveWithinProcess) {
    Env* env = Env::Default();
    const std::string path = TestPath("LOCK");

    std::unique_ptr<FileLock> first;
    ASSERT_OK(env->LockFile(path, &first));
    ASSERT_NE(nullptr, first);

    // flock 是与"打开文件描述"绑定的。
    // 即使是同一个进程，新开一个 fd 再次加锁也会失败 ——
    // 这正是我们要的行为：它能挡住重复打开同一个数据库目录。
    std::unique_ptr<FileLock> second;
    const Status s = env->LockFile(path, &second);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(nullptr, second);

    ASSERT_OK(env->UnlockFile(first.get()));

    // 解锁之后应能重新获得锁
    std::unique_ptr<FileLock> third;
    ASSERT_OK(env->LockFile(path, &third));
    ASSERT_OK(env->UnlockFile(third.get()));

    env->DeleteFile(path);
}

// ---------------------------------------------------------------------------
// 时间与线程
// ---------------------------------------------------------------------------

TEST(EnvTest, NowMicrosIsMonotonic) {
    Env* env = Env::Default();

    const uint64_t t1 = env->NowMicros();
    env->SleepForMicroseconds(2000);
    const uint64_t t2 = env->NowMicros();
    env->SleepForMicroseconds(2000);
    const uint64_t t3 = env->NowMicros();

    // 单调性：时间绝不应倒退（这是 steady_clock 保证的）
    EXPECT_GE(t2, t1);
    EXPECT_GE(t3, t2);

    // 睡眠时长应大致准确（下限放宽以容忍调度抖动）
    EXPECT_GE(t3 - t1, 2000u);
}

TEST(EnvTest, ScheduleRunsTaskOnBackgroundThread) {
    Env* env = Env::Default();

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::thread::id executed_on = std::this_thread::get_id();

    env->Schedule([&] {
        // 必须在**持有锁的情况下**调用 notify_one()。
        //
        // 若改成"先解锁、再 notify"，会引入一个真实的竞争：
        // 通知线程可能仍停留在 notify_one() 内部，
        // 而主线程已经从 wait_for 返回并析构了 cv —— 这是未定义行为。
        //
        // TSAN 报告：
        //   Write  of size 8 at env_test.cpp(析构 cv) -> pthread_cond_destroy
        //   Previous read of size 8 by thread T1      -> pthread_cond_signal
        //
        // 持锁 notify 的代价是被唤醒的线程会立刻阻塞在互斥锁上
        // （"hurry up and wait"，多一次上下文切换）。
        // 生产代码里可以靠"保证 cv 生命周期长于所有通知者"来避免这个开销，
        // 但本例中 cv 是栈上的局部变量，正确性优先。
        std::lock_guard<std::mutex> lock(mu);
        done = true;
        executed_on = std::this_thread::get_id();
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(mu);
    const bool completed =
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; });

    ASSERT_TRUE(completed) << "background task did not run within 5 seconds";
    EXPECT_NE(std::this_thread::get_id(), executed_on)
        << "task must run on the background thread, not the caller";
}

TEST(EnvTest, ScheduledTasksExecuteInOrder) {
    Env* env = Env::Default();

    std::mutex mu;
    std::condition_variable cv;
    std::vector<int> order;

    constexpr int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        env->Schedule([&, i] {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(i);
            if (static_cast<int>(order.size()) == kCount) {
                cv.notify_one();
            }
        });
    }

    std::unique_lock<std::mutex> lock(mu);
    const bool completed = cv.wait_for(lock, std::chrono::seconds(5), [&] {
        return order.size() == static_cast<size_t>(kCount);
    });

    ASSERT_TRUE(completed);

    std::vector<int> expected;
    for (int i = 0; i < kCount; ++i) {
        expected.push_back(i);
    }
    EXPECT_EQ(expected, order) << "tasks must execute in submission order";
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

TEST(EnvTest, LoggerWritesMessagesToFile) {
    Env* env = Env::Default();
    const std::string path = TestPath("env_test.log");

    {
        std::unique_ptr<Logger> logger;
        ASSERT_OK(env->NewLogger(path, &logger));
        logger->Log("first message");
        logger->Log("second message");
    }

    std::unique_ptr<SequentialFile> file;
    ASSERT_OK(env->NewSequentialFile(path, &file));
    char scratch[512];
    Slice result;
    ASSERT_OK(file->Read(sizeof(scratch) - 1, &result, scratch));

    const std::string content = result.ToString();
    EXPECT_NE(std::string::npos, content.find("first message"));
    EXPECT_NE(std::string::npos, content.find("second message"));
    EXPECT_LT(content.find("first message"), content.find("second message"));

    env->DeleteFile(path);
}

}  // namespace
}  // namespace tinystore
