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
