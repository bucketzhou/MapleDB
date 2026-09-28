/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:09:27
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 17:11:50
 * @Description:MemTableIterator：将 MemTable 的 SkipList 迭代器包装成 minidb::Iterator
 * 职责：
 * - 解码 SkipList 中存储的 memtable entry（varint 前缀格式）
 * - key()   返回 InternalKey（user_key + 8字节 tag）
 * - value() 返回 value 字节
 * - Seek()  接受 InternalKey，内部转换为 memtable_key（加 varint32 前缀）再 Seek
 * 生命周期：
 * 构造时对 MemTable 加引用（Ref），析构时释放（Unref），
 * 保证迭代器使用期间底层 SkipList 不被释放。
 */
#pragma once

#include "db/memtable.h"
#include "mapledb/iterator.h"

#include <string>

namespace mapledb {

class MemTableIterator : public Iterator {
public:
    // mem：持有引用，析构时 Unref
    explicit MemTableIterator(MemTable* mem);
    ~MemTableIterator() override;

    bool         Valid() const override;
    void         SeekToFirst() override;
    void         Seek(const Slice& target) override;
    void         Next() override;
    const Slice& key() const override;
    const Slice& value() const override;
    Status       status() const override;

private:
    // 解码 SkipList 当前条目，更新 key_/val_
    void ParseEntry();

    MemTable*                  mem_;      // 所属 MemTable（持有引用）
    MemTable::Table::Iterator* iter_;     // 底层 SkipList 迭代器
    std::string                seek_buf_; // Seek 时的临时 memtable_key 缓冲
    Slice                      key_;      // 当前 InternalKey（指向 SkipList 内存）
    Slice                      val_;      // 当前 value（指向 SkipList 内存）
};

} // namespace mapledb