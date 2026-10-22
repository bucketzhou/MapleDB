/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-17 16:02:58
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-17 16:03:34
 * @Description:
 */
// src/util/comparator.h
// Comparator 抽象接口，避免循环依赖

#pragma once
#include "mapledb/slice.h"

namespace mapledb {

class Comparator {
public:
    virtual ~Comparator()                                     = default;
    virtual int Compare(const Slice& a, const Slice& b) const = 0;
    // 磁盘块在调用比较器前验证键格式；普通字节键没有格式限制。
    virtual bool IsValidKey(const Slice&) const { return true; }
    // 过滤器必须使用与查询一致的键编码；名称区分普通键与去掉 tag 的内部键。
    virtual Slice FilterKey(const Slice& key) const { return key; }
    virtual const char* FilterName() const { return "filter.mapledb.BloomFilter.bytes.v1"; }
};

class BytewiseComparatorImpl : public Comparator {
public:
    int Compare(const Slice& a, const Slice& b) const override { return a.Compare(b); }
};

inline const Comparator* BytewiseComparator() {
    static BytewiseComparatorImpl inst;
    return &inst;
}

} // namespace mapledb
