#pragma once

#include "db/dbformat.h"
#include "db/versionedit.h"
#include "mapledb/iterator.h"

#include <memory>
#include <vector>

namespace mapledb {
class TableCache;

// 修改：L1 及以上同层文件有序且不重叠，只需串联；L0 仍逐文件归并。
// files 由外层归并迭代器持有的 FileSnapshot 固定，必须活到本对象析构之后。
class LevelIterator : public Iterator {
public:
    LevelIterator(TableCache* cache, const std::vector<FileMetaData>* files,
                  const InternalKeyComparator* cmp);
    bool Valid() const override;
    void SeekToFirst() override;
    void Seek(const Slice& target) override;
    void Next() override;
    const Slice& key() const override;
    const Slice& value() const override;
    Status status() const override;

private:
    void OpenFile(size_t index);
    void SkipEmptyFiles();

    TableCache* cache_;
    const std::vector<FileMetaData>* files_;
    const InternalKeyComparator* cmp_;
    size_t index_;
    std::unique_ptr<Iterator> child_; // 每层只持有当前文件，跨文件时归还缓存引用。
    Status status_; // 保留被替换子迭代器的首个错误，不能将读失败当作空文件。
};
} // namespace mapledb
