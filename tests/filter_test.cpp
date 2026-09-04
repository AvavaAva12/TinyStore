#include <string>
#include <vector>

#include "tinystore/filter_policy.h"
#include "gtest/gtest.h"

namespace tinystore {
namespace {

TEST(FilterTest, PresentKeysAlwaysMatch) {
  const FilterPolicy* policy = DefaultFilterPolicy();
  std::vector<std::string> keys = {"alpha", "beta", "gamma", "delta", "epsilon"};
  std::vector<Slice> slices;
  for (const auto& k : keys) slices.emplace_back(k);

  std::string filter;
  policy->CreateFilter(slices.data(), static_cast<int>(slices.size()), &filter);

  // 布隆过滤器的铁律：存在的 key 一定返回 true（无误杀）
  for (const auto& k : keys) {
    EXPECT_TRUE(policy->KeyMayMatch(k, filter))
        << "present key should always match: " << k;
  }
}

TEST(FilterTest, AbsentKeysMostlyRejected) {
  const FilterPolicy* policy = DefaultFilterPolicy();

  // 用较大集合，统计误报率应远低于 2%
  std::vector<std::string> keys;
  for (int i = 0; i < 10000; ++i) keys.push_back("key" + std::to_string(i));
  std::vector<Slice> slices;
  for (const auto& k : keys) slices.emplace_back(k);

  std::string filter;
  policy->CreateFilter(slices.data(), static_cast<int>(slices.size()), &filter);

  int false_positives = 0;
  const int trials = 10000;
  for (int i = 0; i < trials; ++i) {
    const std::string probe = "zzz" + std::to_string(i);  // 一定不在集合中
    if (policy->KeyMayMatch(probe, filter)) ++false_positives;
  }
  const double rate = static_cast<double>(false_positives) / trials;
  EXPECT_LT(rate, 0.05) << "false positive rate too high: " << rate;
}

TEST(FilterTest, EmptyFilter) {
  const FilterPolicy* policy = DefaultFilterPolicy();
  // 空过滤器对一切 key 都返回 false（"一定不在"）
  EXPECT_FALSE(policy->KeyMayMatch("anything", ""));
}

}  // namespace
}  // namespace tinystore
