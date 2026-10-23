#include "db/leveliterator.h"

#include "db/tablecache.h"

#include <cassert>

namespace mapledb {
LevelIterator::LevelIterator(TableCache* cache, const std::vector<FileMetaData>* files,
                             const InternalKeyComparator* cmp) :
    cache_(cache), files_(files), cmp_(cmp), index_(files->size()) {}

bool LevelIterator::Valid() const { return child_ && child_->Valid(); }

void LevelIterator::OpenFile(size_t index) {
    if (child_ && status_.Ok())
        status_ = child_->status();
    child_.reset();
    index_ = index;
    if (index_ < files_->size())
        child_.reset(cache_->NewIterator(&(*files_)[index_]));
}

void LevelIterator::SkipEmptyFiles() {
    while (child_ && !child_->Valid()) {
        // 修改：错误必须停止本层扫描并向外传播，禁止静默跳过缺失或损坏的 SST。
        if (!child_->status().Ok())
            return;
        OpenFile(index_ + 1);
        if (child_)
            child_->SeekToFirst();
    }
}

void LevelIterator::SeekToFirst() {
    OpenFile(0);
    if (child_)
        child_->SeekToFirst();
    SkipEmptyFiles();
}

void LevelIterator::Seek(const Slice& target) {
    // 修改：按 largest 内部键二分定位文件，保留同用户键的 sequence 排序语义。
    size_t left = 0, right = files_->size();
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (cmp_->Compare(Slice((*files_)[mid].largest), target) < 0)
            left = mid + 1;
        else
            right = mid;
    }
    OpenFile(left);
    if (child_)
        child_->Seek(target);
    SkipEmptyFiles();
}

void LevelIterator::Next() {
    assert(Valid());
    child_->Next();
    SkipEmptyFiles();
}

const Slice& LevelIterator::key() const { assert(Valid()); return child_->key(); }
const Slice& LevelIterator::value() const { assert(Valid()); return child_->value(); }
Status LevelIterator::status() const {
    return !status_.Ok() ? status_ : (child_ ? child_->status() : Status::OK());
}
} // namespace mapledb
