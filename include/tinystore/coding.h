#pragma once

#include <cstdint>
#include <string>

#include "tinystore/slice.h"

namespace tinystore {

// ===========================================================================
// 二进制编解码工具
//
// 存储引擎里所有落盘的数据（WAL 记录、SSTable、MANIFEST）都必须有确定的
// 字节格式。编解码层负责两件事：
//   1. 定长整数的小端（little-endian）编码；
//   2. Varint 变长编码。
//
// 【为什么用小端】
// x86-64 / ARM64 主流平台都是小端，用小端编码可以直接 memcpy 到 uint32_t/uint64_t
// 再做数值运算，无需字节序转换。但本实现**刻意采用逐字节移位**而非 memcpy，
// 理由见下方 PutFixed32 的注释。
// ===========================================================================

// ---------------------------------------------------------------------------
// 定长编码
// ---------------------------------------------------------------------------

// 逐字节写入而非 memcpy 的取舍：
//   * 优点：与宿主机字节序无关，代码在任何平台上行为一致；
//   * 代价：无法被编译器优化成单条 mov 指令，理论上比 memcpy 版本慢。
// 在本项目中，定长编码出现在 SSTable 的重启点、Footer 等处，调用频次远低于
// 数据拷贝本身，因此可移植性优先。若未来 perf 显示这里是热点，
// 可加 #if 分支：小端平台走 memcpy 快路径，大端平台走移位慢路径。
inline void EncodeFixed32(char* dst, uint32_t value) {
    dst[0] = static_cast<char>(value & 0xff);
    dst[1] = static_cast<char>((value >> 8) & 0xff);
    dst[2] = static_cast<char>((value >> 16) & 0xff);
    dst[3] = static_cast<char>((value >> 24) & 0xff);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

inline void PutFixed32(std::string* dst, uint32_t value) {
    char buf[sizeof(value)];
    EncodeFixed32(buf, value);
    dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t value) {
    char buf[sizeof(value)];
    EncodeFixed64(buf, value);
    dst->append(buf, sizeof(buf));
}

// 注意：这里必须先把 char 转成 unsigned char 再提升为 uint32_t。
// char 的符号性由平台决定（x86 上是有符号的），若字节值 >= 0x80，
// 直接转成 uint32_t 会做符号扩展，得到 0xffffff80 这样的错误结果。
// 这是二进制解析里最常见的 bug 之一。
inline uint32_t DecodeFixed32(const char* ptr) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(ptr[0]))) |
           (static_cast<uint32_t>(static_cast<unsigned char>(ptr[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(ptr[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(ptr[3])) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result |= static_cast<uint64_t>(static_cast<unsigned char>(ptr[i]))
                  << (8 * i);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Varint 变长编码
//
// 每个字节的低 7 位存数据，最高位（0x80）作为"是否还有后续字节"的续位标志。
// 小端序：先写低位 7 bits。
//
// 用于：长度前缀、SSTable 的共享键长度 / 非共享键长度等。
// 收益：小数值（< 128）只占 1 字节，相比固定 4 字节节省 75% 空间。
// 代价：解码有分支，比定长解码慢；且无法随机访问，只能顺序解析。
// ---------------------------------------------------------------------------

// 最多 5 字节（32 位 = 35 bits / 7，向上取整为 5）
inline char* EncodeVarint32(char* dst, uint32_t v) {
    auto* ptr = reinterpret_cast<unsigned char*>(dst);
    while (v >= 0x80) {
        *ptr = static_cast<unsigned char>(v | 0x80);
        v >>= 7;
        ++ptr;
    }
    *ptr = static_cast<unsigned char>(v);
    return reinterpret_cast<char*>(ptr) + 1;
}

inline void PutVarint32(std::string* dst, uint32_t v) {
    char buf[5];
    char* ptr = EncodeVarint32(buf, v);
    dst->append(buf, static_cast<size_t>(ptr - buf));
}

// 最多 10 字节
inline char* EncodeVarint64(char* dst, uint64_t v) {
    auto* ptr = reinterpret_cast<unsigned char*>(dst);
    while (v >= 0x80) {
        *ptr = static_cast<unsigned char>(v | 0x80);
        v >>= 7;
        ++ptr;
    }
    *ptr = static_cast<unsigned char>(v);
    return reinterpret_cast<char*>(ptr) + 1;
}

inline void PutVarint64(std::string* dst, uint64_t v) {
    char buf[10];
    char* ptr = EncodeVarint64(buf, v);
    dst->append(buf, static_cast<size_t>(ptr - buf));
}

// 从 [p, limit) 解析一个 varint。成功返回"下一个待解析位置"；
// 数据不完整（越界）或编码非法（超过最大字节数）时返回 nullptr。
//
// 返回 nullptr 而不是抛异常或写默认值：解析损坏数据是存储引擎的常态
// （磁盘扇区损坏、写入时被掉电截断），必须是可预期的普通控制流。
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        const uint32_t byte = static_cast<unsigned char>(*p);
        ++p;
        if (byte & 0x80) {
            // 还有后续字节
            result |= ((byte & 0x7f) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

inline const char* GetVarint64Ptr(const char* p, const char* limit,
                                  uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        const uint64_t byte = static_cast<unsigned char>(*p);
        ++p;
        if (byte & 0x80) {
            result |= ((byte & 0x7f) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

// 从 input 头部解析一个 varint，成功后把 input 推进到剩余部分。
inline bool GetVarint32(Slice* input, uint32_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = GetVarint32Ptr(p, limit, value);
    if (q == nullptr) {
        return false;
    }
    *input = Slice(q, static_cast<size_t>(limit - q));
    return true;
}

inline bool GetVarint64(Slice* input, uint64_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = GetVarint64Ptr(p, limit, value);
    if (q == nullptr) {
        return false;
    }
    *input = Slice(q, static_cast<size_t>(limit - q));
    return true;
}

// 计算 varint 编码后的字节数，用于预估缓冲区大小。
inline int VarintLength(uint64_t v) {
    int len = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++len;
    }
    return len;
}

// ---------------------------------------------------------------------------
// 长度前缀的 Slice
//
// 格式：[varint 长度][payload 字节]
// 这是存储引擎里最基础的复合结构，WAL 记录、SSTable 的 entry 都基于它。
// ---------------------------------------------------------------------------

inline void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

inline bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  // 长度字段本身残缺，或 payload 不足 -> 视为数据截断
  if (!GetVarint32(input, &len) || input->size() < len) {
    return false;
  }
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

// 从裸指针解析一个长度前缀 Slice，**不做边界检查**。
//
// 【为什么需要它】
// MemTable 的 SkipList 节点里，key 是一个指向 arena 中「记录缓冲区」的裸指针，
// 记录格式为 [varint(ik_size)][internal_key][varint(val_size)][value]。
// 比较器只需要取出 internal_key 部分来比较，但调用方手里只有一个 const char*，
// 既不知道整条记录的总长度，也不该用 Slice(const char*) 按 strlen 构造
// （记录不是以 '\0' 结尾的字符串，strlen 会越界读到别的内存）。
//
// 因此这里提供一个「信任缓冲区有效」的过载：它只解析 varint 长度，
// 再返回紧随其后的 payload Slice。arena 里的记录一定是完整有效的，
// 所以跳过边界检查既正确又省一次长度探测。
inline Slice GetLengthPrefixedSlice(const char* ptr) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(ptr);
  uint32_t len = 0;
  uint32_t shift = 0;
  while (true) {
    const unsigned char byte = *p++;
    if (byte & 0x80) {
      len |= (static_cast<uint32_t>(byte & 0x7f) << shift);
      shift += 7;
    } else {
      len |= (static_cast<uint32_t>(byte) << shift);
      break;
    }
  }
  return Slice(reinterpret_cast<const char*>(p), len);
}

}  // namespace tinystore
