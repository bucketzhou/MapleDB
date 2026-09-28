/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:17:33
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 17:18:07
 * @Description: MemTableIterator 实现
 */

#include "db/memtableiterator.h"

#include "util/coding.h"

#include <cassert>
#include <cstring>

namespace mapledb {

MemTableIterator::MemTableIterator(MemTable* mem) : mem_(mem), iter_(mem->NewIterator()) {
    mem_->Ref();
}

MemTableIterator::~MemTableIterator() {
    delete iter_;
    mem_->Unref();
}

bool MemTableIterator::Valid() const { return iter_->Valid(); }

void MemTableIterator::SeekToFirst() {
    iter_->SeekToFirst();
    ParseEntry();
}

// target 是 InternalKey（Slice）
// SkipList 比较的是 memtable_key = [varint32(ik_len)][internal_key]
// 需要先把 InternalKey 包装成 memtable_key 才能正确 Seek
void MemTableIterator::Seek(const Slice& target) {
    // 分配 varint32(target.size()) + target.size() 字节
    seek_buf_.resize(VarintLength(target.Size()) + target.Size());
    char* p = EncodeVarint32(&seek_buf_[0], static_cast<uint32_t>(target.Size()));
    memcpy(p, target.Data(), target.Size());
    iter_->Seek(seek_buf_.data());
    ParseEntry();
}

void MemTableIterator::Next() {
    assert(Valid());
    iter_->Next();
    ParseEntry();
}

const Slice& MemTableIterator::key() const {
    assert(Valid());
    return key_;
}

const Slice& MemTableIterator::value() const {
    assert(Valid());
    return val_;
}

Status MemTableIterator::status() const { return Status::OK(); }

// 解码 SkipList 当前条目
// SkipList 存储格式（iter_->key() 返回 const char*）：
//   [varint32: internal_key_len][user_key][tag(8B)][varint32: value_len][value]
void MemTableIterator::ParseEntry() {
    key_ = Slice();
    val_ = Slice();
    if (!iter_->Valid())
        return;

    const char* entry = iter_->key();

    // 解码 varint32(internal_key_len)
    uint32_t    ik_len = 0;
    const char* p      = GetVarint32Ptr(entry, entry + 5, &ik_len);
    if (p == nullptr)
        return;

    // InternalKey = user_key + 8字节 tag
    key_ = Slice(p, ik_len);
    p += ik_len;

    // 解码 varint32(value_len)
    uint32_t val_len = 0;
    p                = GetVarint32Ptr(p, p + 5, &val_len);
    if (p == nullptr)
        return;

    val_ = Slice(p, val_len);
}

} // namespace mapledb