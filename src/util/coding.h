/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-23 14:12:12
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-03 19:27:37
 * @Description: 整数编解码工具：Fixed32/64 和 Varint32/64,会被 WAL、SSTable 大量使用
 */
#pragma once
#include "mapledb/slice.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace mapledb {
// 将 uint32_t 以小端序写入 buf（buf 至少4字节）
inline void EncodeFixed32(char* buf, uint32_t value) {
    // 在小端机器上直接 memcpy 即可（x86/ARM 都是小端）
    // 用 memcpy 避免未对齐访问，强制将char* 类型的buf 转换为uint32_t*
    // 类型，需要buf长度为4字节的倍数
    memcpy(buf, &value, sizeof(value));
}

// 将 uint64_t 以小端序写入 buf（buf 至少8字节）
inline void EncodeFixed64(char* buf, uint64_t value) { memcpy(buf, &value, sizeof(value)); }

// 从 buf 读取小端序 uint32_t
inline uint32_t DecodeFixed32(const char* buf) {
    uint32_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
}

// 从 buf 读取小端序 uint64_t
inline uint64_t DecodeFixed64(const char* buf) {
    uint64_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
}

// 将 Fixed32 追加到 std::string（最常用的封装）
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

// ════════════════════════════════════════════════════════
// Varint 编码：变长整数，节省空间
// 每个字节的最高位是"续标"：1=还有后续字节，0=最后一字节
// 低7位存储实际数据（小端）
//
// 编码示例：
//   1   → 0x01           (1字节)
//   128 → 0x80 0x01      (2字节)
//   300 → 0xAC 0x02      (2字节)
//

// 将 uint32_t 编码为 varint，写入 buf，返回写入后的指针
inline char* EncodeVarint32(char* buf, uint32_t v) {
    // 用无符号类型，避免右移时符号扩展问题
    auto*            ptr = reinterpret_cast<uint8_t*>(buf);
    static const int kB  = 128; // 0x80：续标

    if (v < (1 << 7)) {
        *ptr++ = static_cast<uint8_t>(v);
    } else if (v < (1 << 14)) {
        *ptr++ = static_cast<uint8_t>(v | kB);
        *ptr++ = static_cast<uint8_t>(v >> 7);
    } else if (v < (1 << 21)) {
        *ptr++ = static_cast<uint8_t>(v | kB);
        *ptr++ = static_cast<uint8_t>((v >> 7) | kB);
        *ptr++ = static_cast<uint8_t>(v >> 14);
    } else if (v < (1 << 28)) {
        *ptr++ = static_cast<uint8_t>(v | kB);
        *ptr++ = static_cast<uint8_t>((v >> 7) | kB);
        *ptr++ = static_cast<uint8_t>((v >> 14) | kB);
        *ptr++ = static_cast<uint8_t>(v >> 21);
    } else {
        *ptr++ = static_cast<uint8_t>(v | kB);
        *ptr++ = static_cast<uint8_t>((v >> 7) | kB);
        *ptr++ = static_cast<uint8_t>((v >> 14) | kB);
        *ptr++ = static_cast<uint8_t>((v >> 21) | kB);
        *ptr++ = static_cast<uint8_t>(v >> 28);
    }
    return reinterpret_cast<char*>(ptr);
}

// 将 uint64_t 编码为 varint64
inline char* EncodeVarint64(char* buf, uint64_t v) {
    auto* ptr = reinterpret_cast<uint8_t*>(buf);
    while (v >= 0x80) {
        *ptr++ = static_cast<uint8_t>(v | 0x80);
        v >>= 7;
    }
    *ptr++ = static_cast<uint8_t>(v);
    return reinterpret_cast<char*>(ptr);
}

// 将 varint32 追加到 std::string
inline void PutVarint32(std::string* dst, uint32_t v) {
    char  buf[5]; // varint32 最多5字节
    char* end = EncodeVarint32(buf, v);
    dst->append(buf, static_cast<size_t>(end - buf));
}

// 将 varint64 追加到 std::string
inline void PutVarint64(std::string* dst, uint64_t v) {
    char  buf[10]; // varint64 最多10字节
    char* end = EncodeVarint64(buf, v);
    dst->append(buf, static_cast<size_t>(end - buf));
}

// 将 Slice 以 "varint32长度 + 内容" 格式追加到 dst
// 这是存储 key/value 的标准格式
inline void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<uint32_t>(value.Size()));
    dst->append(value.Data(), value.Size());
}

// ── Varint 解码 ────────────────────────────────────────

// 从 [p, limit) 解码一个 varint32，成功返回新指针，失败返回 nullptr
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);

// 从 [p, limit) 解码一个 varint64
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

// 从 Slice 中解码 varint32（会消耗 input 的前缀）
bool GetVarint32(Slice* input, uint32_t* value);

// 从 Slice 中解码 varint64
bool GetVarint64(Slice* input, uint64_t* value);

// 从 Slice 中解码 "varint32长度前缀 + 内容"，结果放入 result
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// 返回将 v 编码为 varint32 需要的字节数（1~5）
inline int VarintLength(uint64_t v) {
    int len = 1;
    while (v >= 128) {
        v >>= 7;
        len++;
    }
    return len;
}

} // namespace mapledb
