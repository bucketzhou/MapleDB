/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-03 13:26:47
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-03 19:34:50
 * @Description:
 */
#include "dbformat.h"

#include <cstring>

namespace mapledb {
// ── InternalKeyComparator::Compare（核心比较逻辑）──────────

int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
    // 第一步：比较 user_key（去掉尾部 8 字节 tag）
    Slice a_ukey = ExtractUserKey(akey);
    Slice b_ukey = ExtractUserKey(bkey);
    int   r      = a_ukey.Compare(b_ukey);

    if (r != 0)
        return r; // user_key 不同，直接返回
    uint64_t a_tag = DecodeFixed64(akey.Data() + akey.Size() - 8);
    uint64_t b_tag = DecodeFixed64(bkey.Data() + bkey.Size() - 8);

    if (a_tag > b_tag)
        return -1;
    if (a_tag < b_tag)
        return +1;
    return 0;
}

// 内部辅助：从 const char* 解码 length-prefixed Slice（最多读5字节 varint）
// 注意：这里直接内联在 .cpp，不放进 .h 是为了减少不必要的暴露
static Slice GetLengthPrefixedSliceFromChar_impl(const char* data) {
    uint32_t    len = 0;
    const char* p   = data;
    // varint32 最多 5 字节，简单快速解码
    for (int shift = 0; shift <= 28; shift += 7) {
        uint32_t byte = static_cast<uint8_t>(*p++);
        if (byte & 0x80) {
            len |= ((byte & 0x7F) << shift);
        } else {
            len |= (byte << shift);
            return Slice(p, len);
        }
    }
    // 不应到达这里（数据损坏）
    assert(false);
    return Slice();
}

// 为了让 operator() 能用，在这里定义
Slice GetLengthPrefixedSliceFromChar(const char* data) {
    return GetLengthPrefixedSliceFromChar_impl(data);
}

// ── InternalKeyComparator::operator()（MemTable SkipList 使用）

int InternalKeyComparator::operator()(const char* aptr, const char* bptr) const {
    // MemTable 里 SkipList 的 key 带 varint32 前缀（internal_key 的长度）
    // 先解码出 internal_key，再调用 Compare
    Slice a = GetLengthPrefixedSliceFromChar(aptr);
    Slice b = GetLengthPrefixedSliceFromChar(bptr);
    return Compare(a, b);
}

// ── LookupKey 构造函数 ───────────────────────────────────

LookupKey::LookupKey(const Slice& user_key, SequenceNumber sequence) {
    // 计算需要的总字节数：
    //   varint32(internal_key_len) 最多 5 字节
    //   internal_key = user_key_len + 8（tag）
    size_t usize  = user_key.Size();
    size_t needed = usize + 13; // 5（varint 上界）+ 8（tag）= 13，user_key 另算

    char* dst;
    if (needed <= sizeof(space_)) {
        dst = space_; // 短 key：用栈内存
    } else {
        dst = new char[needed + usize]; // 长 key：堆分配
    }

    start_ = dst;

    // 写入 varint32：internal_key_len = user_key_len + 8
    uint32_t internal_key_len = static_cast<uint32_t>(usize + 8);
    dst                       = EncodeVarint32(dst, internal_key_len);

    kstart_ = dst; // internal_key 从这里开始

    // 写入 user_key
    memcpy(dst, user_key.Data(), usize);
    dst += usize;

    // 写入 tag = (seq << 8) | kValueTypeForSeek
    // 用 kValueTypeForSeek(=kTypeValue=1) 是因为相同 seq 下 Value > Deletion
    // 这样 Seek 能找到最新的正常写入（而不会误停在 Deletion 上）
    uint64_t tag = (sequence << 8) | static_cast<uint64_t>(kValueTypeForSeek);
    EncodeFixed64(dst, tag);
    dst += 8;

    end_ = dst;
}

} // namespace mapledb