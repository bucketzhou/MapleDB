/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:20:12
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 17:22:06
 * @Description:MergingIterator：多路归并迭代器（通用版）
 * 接受 N 个 Iterator*，按 InternalKeyComparator 的顺序归并输出。
 * 每次 Next() 推进当前最小 key 的子迭代器，再重新选出最小。
 * 应用场景：
 * DB::NewIterator() 将 MemTable + imm_ + 各层 SSTable 归并成全局有序视图
 * 参考 LevelDB db/merge_iterator.cc MergingIterator
 */
#pragma once

#include "db/dbformat.h"
#include "mapledb/iterator.h"

#include <vector>

namespace mapledb {

class MergingIterator : public Iterator {
public:
    // iters：子迭代器列表，MergingIterator 接管所有权（析构时 delete）
    // cmp  ：InternalKey 比较器（不拥有）
    MergingIterator(std::vector<Iterator*> iters, const InternalKeyComparator* cmp);
    ~MergingIterator() override;

    bool         Valid() const override;
    void         SeekToFirst() override;
    void         Seek(const Slice& target) override;
    void         Next() override;
    const Slice& key() const override;
    const Slice& value() const override;
    Status       status() const override;

private:
    // 扫描所有有效子迭代器，找到 key 最小的，设置 current_
    void FindSmallest();

    std::vector<Iterator*>       iters_;
    const InternalKeyComparator* cmp_;
    int                          current_; // 当前最小迭代器索引，-1 = 无效
};

} // namespace mapledb