#include "tinystore/log_writer.h"
#include "tinystore/log_reader.h"
#include "tinystore/env.h"
#include "tinystore/slice.h"
#include "tinystore/status.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace tinystore {

namespace {

class TestReporter : public log::Reporter {
public:
  size_t corruptions = 0;
  void Corruption(size_t, const Status&) override { ++corruptions; }
};

std::string ReadWholeFile(Env* env, const std::string& fname) {
  std::unique_ptr<SequentialFile> file;
  if (!env->NewSequentialFile(fname, &file).ok()) return "";
  std::string all;
  char buf[4096];
  Slice s;
  while (true) {
    Status st = file->Read(sizeof(buf), &s, buf);
    if (!st.ok()) break;
    if (s.empty()) break;
    all.append(s.data(), s.size());
  }
  return all;
}

}  // namespace

TEST(LogTest, RoundTripSmallAndLarge) {
  Env* env = Env::Default();
  std::string dir;
  ASSERT_TRUE(env->GetTestDirectory(&dir).ok());
  const std::string fname = dir + "/log_test_roundtrip";
  env->DeleteFile(fname);

  // 故意混入：空记录、常规记录、超过一个 block 的大记录（触发 FIRST/MIDDLE/LAST 分片）
  const std::vector<std::string> recs = {
      "",
      "hello",
      std::string(1000, 'x'),
      std::string(log::kBlockSize * 2, 'y'),
  };

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_TRUE(env->NewWritableFile(fname, &wf).ok());
    log::Writer writer(wf.get());
    for (const auto& r : recs) ASSERT_TRUE(writer.AddRecord(Slice(r)).ok());
    ASSERT_TRUE(wf->Close().ok());
  }

  {
    std::unique_ptr<SequentialFile> rf;
    ASSERT_TRUE(env->NewSequentialFile(fname, &rf).ok());
    log::Reader reader(rf.get(), nullptr, /*checksum=*/true);
    std::vector<std::string> got;
    std::string scratch;
    Slice rec;
    while (reader.ReadRecord(&rec, &scratch)) got.push_back(rec.ToString());

    ASSERT_EQ(got.size(), recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
      EXPECT_EQ(got[i], recs[i]) << "record #" << i;
    }
  }
  env->DeleteFile(fname);
}

TEST(LogTest, CorruptionDetected) {
  Env* env = Env::Default();
  std::string dir;
  ASSERT_TRUE(env->GetTestDirectory(&dir).ok());
  const std::string fname = dir + "/log_test_corrupt";
  env->DeleteFile(fname);

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_TRUE(env->NewWritableFile(fname, &wf).ok());
    log::Writer writer(wf.get());
    ASSERT_TRUE(writer.AddRecord(Slice("alpha")).ok());
    ASSERT_TRUE(writer.AddRecord(Slice("beta")).ok());
    ASSERT_TRUE(wf->Close().ok());
  }

  // 翻坏一个数据字节（位于第二条记录的 payload 区），使其 crc 失配
  std::string data = ReadWholeFile(env, fname);
  ASSERT_GT(data.size(), 20u);
  data[20] ^= 0xff;

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_TRUE(env->NewWritableFile(fname, &wf).ok());
    ASSERT_TRUE(wf->Append(Slice(data)).ok());
    ASSERT_TRUE(wf->Close().ok());
  }

  {
    std::unique_ptr<SequentialFile> rf;
    ASSERT_TRUE(env->NewSequentialFile(fname, &rf).ok());
    TestReporter rep;
    log::Reader reader(rf.get(), &rep, /*checksum=*/true);
    std::vector<std::string> got;
    std::string scratch;
    Slice rec;
    while (reader.ReadRecord(&rec, &scratch)) got.push_back(rec.ToString());
    // 至少应检测到一次损坏
    EXPECT_GT(rep.corruptions, 0u);
  }
  env->DeleteFile(fname);
}

}  // namespace tinystore
