/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:21:40
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 17:24:07
 * @Description:
 */

#include "db/mergeiterator.h"

#include <cassert>

namespace mapledb {

MergingIterator::MergingIterator(std::vector<Iterator*> iters, const InternalKeyComparator* cmp) :
    iters_(std::move(iters)), cmp_(cmp), current_(-1) {}

MergingIterator::~MergingIterator() {
    for (auto* it : iters_)
        delete it;
}

bool MergingIterator::Valid() const { return current_ >= 0; }

void MergingIterator::SeekToFirst() {
    for (auto* it : iters_)
        it->SeekToFirst();
    FindSmallest();
}

void MergingIterator::Seek(const Slice& target) {
    for (auto* it : iters_)
        it->Seek(target);
    FindSmallest();
}

void MergingIterator::Next() {
    assert(Valid());
    iters_[current_]->Next();
    FindSmallest();
}

const Slice& MergingIterator::key() const {
    assert(Valid());
    return iters_[current_]->key();
}

const Slice& MergingIterator::value() const {
    assert(Valid());
    return iters_[current_]->value();
}

Status MergingIterator::status() const {
    for (auto* it : iters_) {
        if (!it->status().Ok())
            return it->status();
    }
    return Status::OK();
}

void MergingIterator::FindSmallest() {
    current_ = -1;
    for (int i = 0; i < static_cast<int>(iters_.size()); i++) {
        if (!iters_[i]->Valid())
            continue;
        if (current_ < 0 || cmp_->Compare(iters_[i]->key(), iters_[current_]->key()) < 0) {
            current_ = i;
        }
    }
}

} // namespace mapledb