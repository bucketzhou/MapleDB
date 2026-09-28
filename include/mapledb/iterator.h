/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:00:53
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 17:11:54
 * @Description:通用迭代器抽象接口
 * 参考 LevelDB include/leveldb/iterator.h
 * 迭代器层次：
 * MemTableIterator     ——> 遍历 MemTable，key() 返回 InternalKey
 * TwoLevelIterator     ——> 遍历 SSTable，key() 返回 InternalKey
 * MergingIterator      ——> 多路归并以上两类，key() 返回 InternalKey
 * DBIter               ——> 面向用户，key() 返回 user_key，过滤 tombstone
 */
#pragma once

#include "mapledb/slice.h"
#include "mapledb/status.h"

namespace mapledb {

class Iterator {
public:
    virtual ~Iterator() = default;

    // 当前位置是否有效（指向一个真实的 kv）
    virtual bool Valid() const = 0;

    // 定位到第一个 kv（最小 key）
    virtual void SeekToFirst() = 0;

    // 定位到第一个 key >= target
    //   内部迭代器（MemTable/SST/Merge）：target 为 InternalKey
    //   DBIter：target 为 user_key
    virtual void Seek(const Slice& target) = 0;

    // 向前移动一步（调用前 Valid() 必须为 true）
    virtual void Next() = 0;

    // 当前 key
    //   内部迭代器：InternalKey（user_key + 8字节 tag）
    //   DBIter：user_key（已剥离 tag）
    virtual const Slice& key() const = 0;

    // 当前 value
    virtual const Slice& value() const = 0;

    // 迭代器状态（I/O 错误时非 OK）
    virtual Status status() const = 0;

    // 禁止拷贝
    Iterator(const Iterator&)            = delete;
    Iterator& operator=(const Iterator&) = delete;

protected:
    Iterator() = default;
};

} // namespace mapledb