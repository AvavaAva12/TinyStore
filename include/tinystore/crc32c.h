#pragma once

#include <cstddef>
#include <cstdint>

#include "tinystore/slice.h"

namespace tinystore {
namespace crc32c {

// ===========================================================================
// CRC32C —— Castagnoli 多项式 (0x1EDC6F41) 的循环冗余校验
// ===========================================================================
//
// 【为什么 WAL 需要校验和】
// 写 WAL 是唯一一道"掉电也能恢复"的防线。但磁盘/文件系统会在两种情况下
// 悄悄篡改数据：
//   1. 写到一半掉电：最后一个 block 可能只有半个 record，落盘的是垃圾字节；
//   2. 静默扇区损坏：SSD/HDD 偶尔会返回一个翻转了若干比特的旧数据。
// 没有校验和，恢复时就会把损坏的字节当成合法记录，造成数据错误而非崩溃。
// 校验和让"读到一个坏 record"变成一次明确的 Corruption 错误，可被安全跳过。
//
// 【为什么是 CRC32C 而不是简单的加法/异或校验】
// 单字节校验（如把所有字节 XOR）只能发现奇数个比特翻转，且对"整段被零覆盖"
// 这类常见故障完全无能为力。CRC32C 对突发错误（burst error）极其敏感，
// 能检出所有 <= 32 位的突发错误，是存储系统的事实标准（SSE4.2 甚至有
// 硬件指令 crc32 直接算它）。本项目用手写查表法，可读、可移植、零依赖。
//
// 【为什么不用更长的 CRC64】
// 32 位对"随机损坏"的漏检率约 2^-32，足以满足单机存储引擎；64 位收益有限
// 却多占 4 字节/record 头部。LevelDB / RocksDB 都用 CRC32C，保持一致即可。

// 在已有 crc 基础上追加计算（用于分多次喂入数据，如先喂 type 字节再喂 payload）
uint32_t Extend(uint32_t crc, const char* buf, size_t size);

// 整段数据计算校验和（等价于 Extend(0, ...)）
inline uint32_t Value(const Slice& data) {
  return Extend(0, data.data(), data.size());
}

}  // namespace crc32c
}  // namespace tinystore
