/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-02 18:25:43
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-17 16:04:14
 * @Description:
 *      内部键格式定义：InternalKey、SequenceNumber、ValueType
 *      以及用于查找的 LookupKey 和比较器 InternalKeyComparator
 *      这个文件是整个引擎的"格式契约"，MemTable、WAL、SSTable 都依赖它
 */
#pragma once

#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "util/coding.h"
#include "util/comparator.h"

#include <cstdint>
#include <string>

namespace mapledb {

// ValueType：写操作类型
// 注意：这两个值被编码进磁盘文件，不能修改
enum ValueType : uint8_t {
    kTypeDeletion = 0x0,
    kTypeValue    = 0x1,
};

// Seek 时使用 kTypeValue（因为相同 seq 下，Value > Deletion，
// 用 Value 做 Seek 可以找到 seq 对应的最新写操作）
static const ValueType kValueTypeForSeek = kTypeValue;
// ════════════════════════════════════════════════════════
// SequenceNumber：全局单调递增的写操作序列号
// 每次 Put/Delete 消耗一个序列号
// 56 位有效（低 8 位留给 ValueType），最大值约 7.2×10^16
// ════════════════════════════════════════════════════════
using SequenceNumber                           = uint64_t;
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

// 将 user_key + seq + type 打包成 InternalKey（追加到 result 末尾）
// InternalKey 格式：[user_key bytes][tag = (seq << 8 | type) as uint64 LE]
inline void AppendInternalKey(std::string* result, const Slice& user_key, SequenceNumber seq,
                              ValueType type) {
    result->append(user_key.Data(), user_key.Size());
    uint64_t tag = (seq << 8) | static_cast<uint64_t>(type);
    PutFixed64(result, tag);
}

// 从 InternalKey 中提取 user_key（去掉尾部 8 字节 tag）
inline Slice ExtractUserKey(const Slice& internal_key) {
    assert(internal_key.Size() >= 8);
    return Slice(internal_key.Data(), internal_key.Size() - 8);
}

// 从 InternalKey 尾部解码 SequenceNumber
inline SequenceNumber GetSequenceNumber(const Slice& internal_key) {
    assert(internal_key.Size() >= 8);
    uint64_t tag = DecodeFixed64(internal_key.Data() + internal_key.Size() - 8);
    return tag >> 8;
}

// 从 InternalKey 尾部解码 ValueType
inline ValueType GetValueType(const Slice& internal_key) {
    assert(internal_key.Size() >= 8);
    uint64_t tag = DecodeFixed64(internal_key.Data() + internal_key.Size() - 8);
    return static_cast<ValueType>(tag & 0xff);
}

// ════════════════════════════════════════════════════════
// InternalKeyComparator
// 比较规则：
//   1. user_key 升序（字典序）
//   2. user_key 相同 → SequenceNumber 降序（新版本排前面）
//
// 为什么序列号要降序？
//   查询时用 Seek(user_key + kMaxSeq) 定位，
//   第一条 user_key 匹配的记录就是最新版本，直接读出，无需继续扫描。
// ════════════════════════════════════════════════════════
// class InternalKeyComparator {
class InternalKeyComparator : public Comparator {
public:
    bool IsValidKey(const Slice& key) const override {
        // 修复：损坏的内部键必须在比较前拒绝，防止提取 tag 时断言或越界。
        return key.Size() >= 8 && GetValueType(key) <= kTypeValue;
    }
    // 比较两个 InternalKey（以 Slice 形式传入）
    int Compare(const Slice& a, const Slice& b) const override;

    // 函数调用运算符重载，方便作为 SkipList 的 Comparator 模板参数
    // 注意：MemTable 里 SkipList 的 key 是 const char*（带 varint 前缀）
    // 这里比较的是"完整 MemTable key"（先解码出 InternalKey 再比较）
    int operator()(const char* a, const char* b) const;
};

// ════════════════════════════════════════════════════════
// LookupKey：查询时使用的临时对象
// 内部同时保存三种视图，避免重复编码：
//   memtable_key = [varint32: internal_key_len][internal_key]
//   internal_key = [user_key][tag]
//   user_key     = [user_key]
// ════════════════════════════════════════════════════════

class LookupKey {
public:
    // 构造查找键：user_key + 指定 sequence（通常用 kMaxSequenceNumber）
    LookupKey(const Slice& user_key, SequenceNumber sequence);

    ~LookupKey() {
        if (start_ != space_)
            delete[] start_; // 释放堆上分配的内存
    }
    // 禁止拷贝，有裸指针分配
    LookupKey(const LookupKey&)            = delete;
    LookupKey& operator=(const LookupKey&) = delete;

    // 返回完整的 MemTable key（用于 SkipList::Seek）
    Slice memtable_key() const { return Slice(start_, end_ - start_); }

    // 返回 InternalKey 部分（用于 SSTable 查找）
    Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

    // 返回 user_key 部分
    Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

private:
    // 内存布局（连续存储）：
    //   [start_]  varint32(internal_key_len)
    //   [kstart_] user_key bytes
    //             tag (uint64, 8 bytes)
    //   [end_]
    const char* start_;  // memtable_key 起始（varint 前缀）
    const char* kstart_; // internal_key 起始（user_key 起始）
    const char* end_;    // 结束位置

    // 对于短 key，用栈内存避免堆分配（200 字节覆盖大多数场景）
    char space_[200];
};
} // namespace mapledb
