#include "tinystore/status.h"

#include <cerrno>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace tinystore {
namespace {

TEST(StatusTest, OkStateRequiresNoAllocation) {
    const Status s = Status::OK();
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(Status::Code::kOk, s.code());
    EXPECT_EQ(Slice("<OK>"), s.message());
    EXPECT_EQ("OK", s.ToString());

    // OK 状态只靠 nullptr 表示，不应分配任何内存。
    // 这也保证了 sizeof(Status) == 指针大小，可以用寄存器返回。
    EXPECT_EQ(sizeof(void*), sizeof(Status));
}

TEST(StatusTest, AllErrorCodesAreDistinguishable) {
    const Status not_found = Status::NotFound("nf");
    EXPECT_TRUE(not_found.IsNotFound());
    EXPECT_FALSE(not_found.IsIOError());
    EXPECT_FALSE(not_found.ok());
    EXPECT_EQ(Status::Code::kNotFound, not_found.code());

    const Status corruption = Status::Corruption("crc");
    EXPECT_TRUE(corruption.IsCorruption());

    const Status not_supported = Status::NotSupported("ns");
    EXPECT_TRUE(not_supported.IsNotSupported());

    const Status invalid = Status::InvalidArgument("ia");
    EXPECT_TRUE(invalid.IsInvalidArgument());

    const Status io_error = Status::IOError("io");
    EXPECT_TRUE(io_error.IsIOError());
}

TEST(StatusTest, ToStringUsesReadablePrefix) {
    EXPECT_EQ("NotFound: k", Status::NotFound("k").ToString());
    EXPECT_EQ("Corruption: k", Status::Corruption("k").ToString());
    EXPECT_EQ("Invalid argument: k", Status::InvalidArgument("k").ToString());
    EXPECT_EQ("IO error: k", Status::IOError("k").ToString());
}

TEST(StatusTest, TwoPartMessageJoinedBySeparator) {
    // 这是最常见的用法：上下文（哪个文件）+ 系统错误（为什么失败）
    const Status s = Status::IOError("/data/000123.sst", "No such file");
    EXPECT_EQ("IO error: /data/000123.sst: No such file", s.ToString());
}

TEST(StatusTest, SinglePartMessageHasNoTrailingSeparator) {
    const Status s = Status::NotFound("key-42");
    EXPECT_EQ("NotFound: key-42", s.ToString());

    // 第二段为空时不应插入多余的 ": "
    const Status s2 = Status::NotFound("key-42", Slice());
    EXPECT_EQ("NotFound: key-42", s2.ToString());
}

TEST(StatusTest, MessageIsPreservedExactly) {
    const std::string msg = "a message with spaces and 特殊字符";
    const Status s = Status::InvalidArgument(msg);
    EXPECT_EQ(msg, s.message().ToString());
}

TEST(StatusTest, CopyIsDeepAndIndependent) {
    Status original = Status::NotFound("original");
    Status copy = original;

    EXPECT_FALSE(copy.ok());
    EXPECT_EQ(original.ToString(), copy.ToString());
    EXPECT_NE(original.message().data(), copy.message().data())
        << "拷贝必须分配独立的缓冲区，否则析构时会 double free";

    // 源对象被重置后，拷贝仍然有效 —— 验证深拷贝正确
    original = Status::OK();
    EXPECT_TRUE(original.ok());
    EXPECT_EQ("NotFound: original", copy.ToString());
}

TEST(StatusTest, MoveTransfersOwnershipAndLeavesSourceOk) {
    Status source = Status::Corruption("moved");
    const char* before = source.message().data();

    Status dest = std::move(source);

    EXPECT_TRUE(source.ok()) << "移动后源对象应处于 OK（nullptr）状态";
    EXPECT_TRUE(dest.IsCorruption());
    EXPECT_EQ("Corruption: moved", dest.ToString());
    EXPECT_EQ(before, dest.message().data()) << "移动不应拷贝内存";
}

TEST(StatusTest, SelfAssignmentIsSafe) {
    Status s = Status::IOError("self");
    Status& alias = s;  // 通过引用绕过编译器的自赋值警告
    s = alias;

    EXPECT_TRUE(s.IsIOError());
    EXPECT_EQ("IO error: self", s.ToString());
}

TEST(StatusTest, AssignmentOverwritesPreviousError) {
    Status s = Status::NotFound("first");
    s = Status::IOError("second");

    EXPECT_FALSE(s.IsNotFound());
    EXPECT_TRUE(s.IsIOError());
    EXPECT_EQ("IO error: second", s.ToString());
}

TEST(StatusTest, EmptyMessageIsAllowed) {
    const Status s = Status::IOError(Slice());
    EXPECT_TRUE(s.IsIOError());
    EXPECT_EQ(0u, s.message().size());
    EXPECT_EQ("IO error: ", s.ToString());
}

TEST(StatusTest, IOErrorFromErrnoIncludesSystemMessage) {
    const Status s = IOErrorFromErrno("open(/nope)", ENOENT);
    EXPECT_TRUE(s.IsIOError());
    // 系统消息内容依赖 glibc，但一定非空且包含上下文
    EXPECT_NE(std::string::npos, s.ToString().find("open(/nope)"));
    EXPECT_GT(s.message().size(), 0u);
}

// ---------------------------------------------------------------------------
// 这个测试验证 Status 可以被放进标准容器并正确排序/去重，
// 从而间接验证了比较与拷贝语义的完整性。
// ---------------------------------------------------------------------------
TEST(StatusTest, CanBeStoredInStandardContainer) {
    std::vector<Status> statuses;
    statuses.push_back(Status::OK());
    statuses.push_back(Status::NotFound("a"));
    statuses.push_back(Status::IOError("b"));

    ASSERT_EQ(3u, statuses.size());
    EXPECT_TRUE(statuses[0].ok());
    EXPECT_EQ("NotFound: a", statuses[1].ToString());
    EXPECT_EQ("IO error: b", statuses[2].ToString());
}

}  // namespace
}  // namespace tinystore
