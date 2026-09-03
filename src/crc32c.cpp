#include "tinystore/crc32c.h"

#include <array>
#include <cstdint>

namespace tinystore {
namespace crc32c {

namespace {

// CRC32C 查表（标准反射 / LSB-first 表驱动算法）。
//
// 多项式用 0x82F63B78 —— 这是 Castagnoli 多项式 0x1EDC6F41 的"按位反射"形式，
// 因为绝大多数硬件与软件实现都用 LSB-first（每字节从最低位吃起）。SSE4.2 的
// _mm_crc32_u8 也是这个反射约定，因此本项目与之保持一致，方便将来用硬件指令替代。
//
// 约定：init = 0xFFFFFFFF，xorout = 0xFFFFFFFF（CRC 的工业标准做法）。
//   即 Extend 先做 crc ^= 0xFFFFFFFF 进入寄存器，结束时再 ^= 0xFFFFFFFF 吐出。
// 因此 "123456789" 的校验和应为 0xE3069283（CRC32C 的标准校验向量）。
//
// 用 std::array 承载查表结果：裸数组无法从 lambda 按值返回，std::array 可以；
// 静态局部 + magic static 保证只构造一次且线程安全。
const uint32_t* Table() {
  static const std::array<uint32_t, 256> table = []() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      // 反射算法：从最低位起，poly 用反射形式 0x82F63B78
      uint32_t crc = i;
      for (int k = 0; k < 8; ++k) {
        crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
      }
      t[i] = crc;
    }
    return t;
  }();
  return table.data();
}

}  // namespace

uint32_t Extend(uint32_t crc, const char* buf, size_t size) {
  const uint32_t* const table = Table();
  // 进入寄存器前反转 init；终点反转 xorout。两处都一致，分段 Extend 才能正确串联。
  uint32_t l = crc ^ 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    l = table[(l ^ static_cast<uint8_t>(buf[i])) & 0xffu] ^ (l >> 8);
  }
  return l ^ 0xffffffffu;
}

}  // namespace crc32c
}  // namespace tinystore
