/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:30:21
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 17:32:24
 * @Description:
 * DBIter：面向用户的 DB 迭代器
 *
 * 包装内部 MergingIterator，实现以下功能：
 *   1. 快照隔离：只返回 seq <= snapshot 的写入
 *   2. Tombstone 过滤：kTypeDeletion 的 key 对用户不可见
 *   3. 版本去重：相同 user_key 只返回 seq 最大的可见版本
 *   4. key()   返回 user_key（剥离 InternalKey tag）
 *   5. value() 返回 value（tombstone 已过滤）
 *
 * 范围查询示例：
 *   auto* iter = db->NewIterator();
 *   for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
 *       use(iter->key(), iter->value());
 *   }
 *   delete iter;
 *
 * 参考 LevelDB db/db_iter.cc DBIter
 */

#pragma once

#include "db/dbformat.h"
#include "mapledb/iterator.h"

#include <string>

namespace mapledb {

class DBIter : public Iterator {
public:
    // input    : 内部多路归并迭代器（接管所有权，析构时 delete）
    // sequence : 快照序列号，只可见 seq <= sequence 的写入
    // icmp     : InternalKey 比较器（不拥有）
    DBIter(Iterator* input, SequenceNumber sequence, const InternalKeyComparator& icmp);
    ~DBIter() override;

    bool Valid() const override;
    void SeekToFirst() override;

    // target 为 user_key（不含 tag）
    // 定位到第一个 user_key >= target 且可见的条目
    void Seek(const Slice& target) override;

    void Next() override;

    // 返回 user_key（已剥离 InternalKey 的 8 字节 tag）
    const Slice& key() const override;

    // 返回 value（tombstone 已过滤）；借用底层数据，有效期到下次移动或析构。
    const Slice& value() const override;

    Status status() const override;

private:
    // 从 input_ 当前位置向前推进，找到下一个对用户可见的 user_key
    // skipping   : true = 需要跳过 skip_key 及其所有旧版本
    // 修改：直接复用 saved_key_ 保存待跳过的用户键，避免每次 Next 再复制一份。
    void FindNextUserEntry(bool skipping);

    Iterator*                    input_;    // 内部多路归并迭代器
    const SequenceNumber         sequence_; // 快照序列号
    const InternalKeyComparator& icmp_;
    bool                         valid_;       // 当前是否有效
    std::string                  saved_key_;   // 当前 user_key 的副本
    Slice                        key_slice_;   // 指向 saved_key_
    Slice                        value_slice_; // 借用 input_ 当前条目的 value
};

} // namespace mapledb
