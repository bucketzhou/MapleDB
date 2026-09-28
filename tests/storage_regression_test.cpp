#include "mapledb/db.h"
#include "db/dbformat.h"
#include "db/tablecache.h"
#include "table/tablebuilder.h"
#include "util/cache.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace mapledb;
namespace fs = std::filesystem;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void OK(const Status& s) {
    if (!s.Ok()) throw std::runtime_error(s.ToString());
}
static std::string Key(int i) {
    char b[32];
    snprintf(b, sizeof b, "key%08d", i);
    return b;
}
static std::unique_ptr<DB> Open(const fs::path& path, const Options& opts) {
    DB* db = nullptr;
    OK(DB::Open(opts, path.string(), &db));
    return std::unique_ptr<DB>(db);
}
static void CheckGet(DB* db, int key, const std::string& value) {
    std::string got;
    OK(db->Get(Key(key), &got));
    Check(got == value, "incorrect Get value");
}

static void ConcurrentTableReads(const fs::path& root) {
    Options opts;
    opts.block_size = 1024;
    const auto path = root / "shared-table.ldb";
    const int n = 1024;
    {
        TableBuilder builder(path.string(), opts);
        for (int i = 0; i < n; ++i)
            builder.Add(Slice(Key(i)), Slice(std::string(128, 'v') + Key(i)));
        OK(builder.Finish());
    }
    Table* opened = nullptr;
    OK(Table::Open(path.string(), opts, &opened));
    std::unique_ptr<Table> table(opened);
    struct Result { std::string key, value; };
    std::atomic<bool> good{true};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) readers.emplace_back([&, t] {
        for (int j = 0; j < 3000; ++j) {
            const std::string key = Key((j * 997 + t * 131) % n);
            Result result;
            Status s = table->InternalGet(Slice(key), &result,
                [](void* arg, const Slice& found_key, const Slice& found_value) {
                    auto* out = static_cast<Result*>(arg);
                    out->key = found_key.ToString();
                    out->value = found_value.ToString();
                });
            if (!s.Ok() || result.key != key || result.value != std::string(128, 'v') + key)
                good = false;
        }
    });
    for (auto& reader : readers) reader.join();
    Check(good.load(), "shared Table concurrent reads returned incorrect bytes");
}

static void IteratorCacheLifetimeAndComparator(const fs::path& root) {
    const auto path = root / "tables";
    fs::create_directories(path);
    Options opts;
    opts.block_size = 32;
    InternalKeyComparator cmp;
    {
        TableBuilder builder((path / "000001.ldb").string(), opts);
        for (uint64_t seq : {300, 200, 100}) {
            std::string key;
            AppendInternalKey(&key, Slice("same"), seq, kTypeValue);
            builder.Add(Slice(key), Slice(std::to_string(seq)));
        }
        OK(builder.Finish());
    }
    TableCache cache(path.string(), opts, &cmp, 0); // Release 后会立即销毁未持有的条目
    FileMetaData file;
    file.number = 1;
    file.file_size = fs::file_size(path / "000001.ldb");
    std::unique_ptr<Iterator> it(cache.NewIterator(&file));
    cache.Evict(1);
    std::string target;
    AppendInternalKey(&target, Slice("same"), 250, kTypeValue);
    it->Seek(Slice(target));
    Check(it->Valid(), "iterator lost Table after eviction");
    Check(it->value().ToString() == "200", "iterator used wrong internal comparator");
    OK(it->status());
    file.number = 999;
    std::unique_ptr<Iterator> missing(cache.NewIterator(&file));
    Check(!missing->status().Ok(), "missing SSTable error was hidden");
}

static void OpenErrors(const fs::path& root) {
    const auto empty = root / "empty";
    fs::create_directory(empty);
    Options opts;
    opts.create_if_missing = false;
    DB* db = nullptr;
    Check(!DB::Open(opts, empty.string(), &db).Ok() && db == nullptr,
          "create_if_missing=false created a database");
    opts.create_if_missing = true;
    opts.block_size = 0;
    Check(!DB::Open(opts, empty.string(), &db).Ok() && db == nullptr,
          "zero block size accepted");
    opts.block_size = 4096;
    std::ofstream file(root / "not-a-directory");
    file.close();
    Check(!DB::Open(opts, (root / "not-a-directory" / "child").string(), &db).Ok() && db == nullptr,
          "filesystem failure did not return Status");
}

static void RepeatedRecovery(const fs::path& root) {
    Options opts;
    const auto path = root / "recovery";
    {
        auto db = Open(path, opts);
        for (int i = 0; i < 200; ++i) OK(db->Put(Key(i), "tail"));
    }
    for (int reopen = 0; reopen < 3; ++reopen) {
        auto db = Open(path, opts);
        for (int i = 0; i < 200; ++i) CheckGet(db.get(), i, "tail");
    }
    opts.writer_buffer_size = 64 * 1024;
    const auto large = root / "multi-wal";
    const std::string value(512, 'v');
    {
        auto db = Open(large, opts);
        for (int i = 0; i < 3000; ++i) OK(db->Put(Key(i), value));
    }
    for (int reopen = 0; reopen < 2; ++reopen) {
        auto db = Open(large, opts);
        for (int i = 0; i < 3000; ++i) CheckGet(db.get(), i, value);
    }
}

static void SnapshotDuringCompaction(const fs::path& root) {
    Options opts;
    opts.writer_buffer_size = 64 * 1024;
    opts.l0_compaction_trigger = 2;
    std::unique_ptr<Cache> cache(NewLRUCache(64 * 1024));
    opts.block_cache = cache.get();
    auto db = Open(root / "snapshot", opts);
    const int n = 3000;
    const std::string old_value(96, 'a');
    for (int i = 0; i < n; ++i) OK(db->Put(Key(i), old_value));
    OK(db->Flush());
    std::unique_ptr<Iterator> old(db->NewIterator());
    // 尚未 Seek 的迭代器也必须保留旧 SSTable。
    for (int i = 0; i < n; ++i) {
        if (i % 3 == 0) OK(db->Delete(Key(i)));
        else OK(db->Put(Key(i), "new"));
    }
    OK(db->Flush());
    int count = 0;
    for (old->SeekToFirst(); old->Valid(); old->Next(), ++count) {
        Check(old->key().ToString() == Key(count), "snapshot key changed");
        Check(old->value().ToString() == old_value, "snapshot value changed");
    }
    OK(old->status());
    Check(count == n, "snapshot lost keys");
    old.reset();
    for (int i = 0; i < n; ++i) {
        std::string value;
        const Status s = db->Get(Key(i), &value);
        if (i % 3 == 0) Check(s.IsNotFound(), "deleted key reappeared");
        else { OK(s); Check(value == "new", "overwrite lost"); }
    }
}

static void ConcurrentReadsAndFlush(const fs::path& root) {
    Options opts;
    opts.writer_buffer_size = 64 * 1024;
    opts.l0_compaction_trigger = 2;
    std::unique_ptr<Cache> cache(NewLRUCache(16 * 1024));
    opts.block_cache = cache.get();
    auto db = Open(root / "concurrent", opts);
    const int n = 2000;
    const std::string value(256, 'x');
    for (int i = 0; i < n; ++i) OK(db->Put(Key(i), value));
    OK(db->Flush());
    cache->Prune();
    std::atomic<bool> good{true};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) readers.emplace_back([&, t] {
        for (int j = 0; j < 5000; ++j) {
            std::string got;
            const auto s = db->Get(Key((j * 997 + t * 131) % n), &got);
            if (!s.Ok() || got != value) good = false;
            if (j % 100 == 0) cache->Prune();
        }
    });
    for (int i = n; i < 2 * n; ++i) OK(db->Put(Key(i), value));
    OK(db->Flush());
    for (auto& thread : readers) thread.join();
    Check(good.load(), "concurrent read returned incorrect bytes");
}

int main() {
    const auto root = fs::temp_directory_path() / ("mapledb-regression-" + std::to_string(getpid()));
    try {
        fs::create_directories(root);
        ConcurrentTableReads(root);
        OpenErrors(root);
        RepeatedRecovery(root);
        SnapshotDuringCompaction(root);
        ConcurrentReadsAndFlush(root);
        IteratorCacheLifetimeAndComparator(root);
        fs::remove_all(root);
        std::cout << "storage regression tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << " (data: " << root << ")\n";
        return 1;
    }
}
