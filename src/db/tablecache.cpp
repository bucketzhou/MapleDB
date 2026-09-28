// src/db/table_cache.cpp
// TableCache 实现
//
// 缓存 key  : 8 字节 file_number（EncodeFixed64）
// 缓存 value: TableAndFile*（包含一个 Table*）
// 缓存 charge: 1（每个 Table 条目计为 1 个单位，entries 即条目上限）

#include "db/tablecache.h"

#include "util/coding.h"

#include <cassert>
#include <cstdio>

namespace mapledb {

// ── 缓存条目结构 ─────────────────────────────────────────────────

struct TableAndFile {
    Table* table;
};

class ErrorIterator final : public Iterator {
public:
    explicit ErrorIterator(Status status) : status_(std::move(status)) {}
    bool Valid() const override { return false; }
    void SeekToFirst() override {}
    void Seek(const Slice&) override {}
    void Next() override {}
    const Slice& key() const override { return empty_; }
    const Slice& value() const override { return empty_; }
    Status status() const override { return status_; }
private:
    Status status_;
    Slice empty_;
};

// 缓存淘汰时调用：关闭并释放 Table 对象
static void DeleteEntry(const Slice& /*key*/, void* value) {
    TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
    delete tf->table;
    delete tf;
}

// ── 构造/析构 ────────────────────────────────────────────────────

TableCache::TableCache(const std::string& dbname, const Options& options, const Comparator* cmp,
                       int entries) :
    dbname_(dbname), options_(options), cmp_(cmp),
    cache_(NewLRUCache(static_cast<size_t>(entries))) {}

TableCache::~TableCache() { delete cache_; }

// ── 内部工具 ────────────────────────────────────────────────────

std::string TableCache::TableFileName(uint64_t number) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "/%06llu.ldb", static_cast<unsigned long long>(number));
    return dbname_ + buf;
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle) {
    // 用 8 字节 file_number 作为 cache key
    char key_buf[sizeof(uint64_t)];
    EncodeFixed64(key_buf, file_number);
    Slice key(key_buf, sizeof(key_buf));

    *handle = cache_->Lookup(key);
    if (*handle != nullptr) {
        // 缓存命中，直接返回
        return Status::OK();
    }

    // 缓存锁只保护单次操作；串行化 miss/open/insert 并二次检查，避免重复打开同一表。
    std::lock_guard<std::mutex> lock(open_mutex_);
    *handle = cache_->Lookup(key);
    if (*handle != nullptr)
        return Status::OK();

    // 缓存未命中：打开 SSTable 文件
    std::string fname = TableFileName(file_number);
    Table*      table = nullptr;
    Status      s     = Table::Open(fname, options_, cmp_, &table);
    if (!s.Ok())
        return s;

    auto* tf  = new TableAndFile;
    tf->table = table;
    // charge = 1 表示每个 Table 占一个"条目"单位
    *handle = cache_->Insert(key, tf, 1, DeleteEntry);

    (void) file_size; // 教学版不使用文件大小验证
    return Status::OK();
}

// ── NewIterator ──────────────────────────────────────────────────

Table::Iterator* TableCache::NewIterator(const FileMetaData* meta, Table** tableptr) {
    Cache::Handle* handle = nullptr;
    Status         s      = FindTable(meta->number, meta->file_size, &handle);
    if (!s.Ok()) {
        if (tableptr)
            *tableptr = nullptr;
        return new ErrorIterator(s);
    }

    Table*           table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    Table::Iterator* iter  = table->NewIterator();

    // 原先立即 Release 会让淘汰释放迭代器依赖的 Table；把引用交给迭代器持有。
    // 派生类释放 DataBlock 后，cleanup 才允许关闭所属 Table。
    iter->RegisterCleanup([](void* cache, void* h) {
        static_cast<Cache*>(cache)->Release(static_cast<Cache::Handle*>(h));
    }, cache_, handle);

    if (tableptr)
        *tableptr = table;
    return iter;
}

// ── Get ──────────────────────────────────────────────────────────

Status TableCache::Get(const FileMetaData* meta, const Slice& internal_key, void* arg,
                       void (*handle_result)(void*, const Slice&, const Slice&)) {
    Cache::Handle* handle = nullptr;
    Status         s      = FindTable(meta->number, meta->file_size, &handle);
    if (!s.Ok())
        return s;

    Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s            = table->InternalGet(internal_key, arg, handle_result);
    cache_->Release(handle);
    return s;
}

// ── Evict ────────────────────────────────────────────────────────

void TableCache::Evict(uint64_t file_number) {
    char key_buf[sizeof(uint64_t)];
    EncodeFixed64(key_buf, file_number);
    cache_->Erase(Slice(key_buf, sizeof(key_buf)));
}

} // namespace mapledb
