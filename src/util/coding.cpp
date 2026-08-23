/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-23 15:17:42
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-23 15:41:46
 * @Description:Varint 解码实现（编码是 inline，解码稍复杂放在 .cpp）
 */

#include "coding.h"

namespace mapledb {

// 处理2-5字节的 varint32
static const char* GetVarint32PtrFallback(const char* p, const char* limit, uint32_t* value) {
    uint32_t result = 0;
    for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        uint32_t byte = static_cast<uint8_t>(*p++);
        if (byte & 0x80) {
            // 续标为1，还有后续字节
            result |= ((byte & 0x7F) << shift);
        } else {
            // 续标为0，这是最后一字节
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr; // 解码失败（数据截断或格式错误）
}

// ── Varint32 解码 ─────────────────────────────────────

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
    if (p < limit) {
        uint32_t result = static_cast<uint8_t>(*p);
        if ((result & 0x80) == 0) {
            // 最高位为0，只有1字节
            *value = result;
            return p + 1;
        }
    }
    // 超过1字节，进入慢速路径
    return GetVarint32PtrFallback(p, limit, value);
}

// ── Varint64 解码 ─────────────────────────────────────

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        uint64_t byte = static_cast<uint8_t>(*p++);
        if (byte & 0x80) {
            result |= ((byte & 0x7F) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

// ── 从 Slice 解码（会消耗 Slice 的前缀）─────────────────

bool GetVarint32(Slice* input, uint32_t* value) {
    const char* p     = input->Data();
    const char* limit = p + input->Size();
    const char* q     = GetVarint32Ptr(p, limit, value);
    if (q == nullptr)
        return false;
    *input = Slice(q, static_cast<size_t>(limit - q));
    return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
    const char* p     = input->Data();
    const char* limit = p + input->Size();
    const char* q     = GetVarint64Ptr(p, limit, value);
    if (q == nullptr)
        return false;
    *input = Slice(q, static_cast<size_t>(limit - q));
    return true;
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len;
    if (GetVarint32(input, &len) && input->Size() >= len) {
        *result = Slice(input->Data(), len);
        input->RemovePrefix(len);
        return true;
    }
    return false;
}

} // namespace mapledb