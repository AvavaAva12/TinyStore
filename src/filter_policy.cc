#include "tinystore/filter_policy.h"

#include <cstdint>
#include <cstring>

namespace tinystore {
namespace {

// 一个简单但足够分散的 32 位哈希（FNV-1a 变体），用于把 key 映射到布隆位。
uint32_t BloomHash(const Slice& key) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < key.size(); ++i) {
    h ^= static_cast<uint8_t>(key[i]);
    h *= 16777619u;
  }
  return h;
}

// 标准 LevelDB 布隆过滤器实现：
//   * 最佳探测次数 k ≈ bits_per_key * ln2 ≈ 0.69 * bits_per_key；
//   * 过滤器末尾额外存一个字节表示 k，方便读取端复算位；
//   * 用 (h, h += rotate(h)) 的多重哈希避免多次独立哈希的开销。
class BloomFilterPolicy final : public FilterPolicy {
public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {}

  const char* Name() const override { return "tinystore.BuiltinBloomFilter"; }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    // 0.69 ≈ ln(2)
    size_t bits = static_cast<size_t>(n) * bits_per_key_;
    if (bits < 64) bits = 64;
    std::vector<char> array((bits + 7) / 8, 0);
    bits = array.size() * 8;

    int k = static_cast<int>(bits_per_key_ * 0.69);
    if (k < 1) k = 1;
    if (k > 30) k = 30;

    for (int i = 0; i < n; ++i) {
      uint32_t h = BloomHash(keys[i]);
      const uint32_t delta = (h >> 17) | (h << 15);  // 循环移位，制造独立探测
      for (int j = 0; j < k; ++j) {
        const uint32_t bitpos = h % static_cast<uint32_t>(bits);
        array[bitpos / 8] |= (1 << (bitpos % 8));
        h += delta;
      }
    }
    array.push_back(static_cast<char>(k));  // 末尾字节记录探测次数
    dst->assign(array.data(), array.size());
  }

  bool KeyMayMatch(const Slice& key, const Slice& filter) const override {
    const size_t len = filter.size();
    if (len < 2) return false;  // 长度不足以含 k 字节，按"不在"处理

    const char k = filter[len - 1];
    if (k > 30) return true;  // k 异常，保守返回"可能在"，不误杀

    const size_t bits = (len - 1) * 8;
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int j = 0; j < static_cast<int>(k); ++j) {
      const uint32_t bitpos = h % static_cast<uint32_t>(bits);
      if ((filter[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }

private:
  int bits_per_key_;
};

}  // namespace

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

const FilterPolicy* DefaultFilterPolicy() {
  // 函数内静态对象：静态存储期，进程退出时销毁，不会泄漏，也没有堆分配。
  // 多个线程首次同时进入时，C++11 保证初始化只发生一次（magic statics）。
  static const BloomFilterPolicy kDefault(10);
  return &kDefault;
}

}  // namespace tinystore
