#include "mapledb/db.h"
#include "util/cache.h"
#include "../benchmarks/parallel_runner.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace mapledb;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void OK(const Status& status) {
    if (!status.Ok()) throw std::runtime_error(status.ToString());
}
static std::string Key(size_t id) {
    char bytes[32];
    std::snprintf(bytes, sizeof bytes, "key%08zu", id);
    return bytes;
}
static std::string Value(size_t id, size_t generation) {
    return std::to_string(generation) + ":" + Key(id) + ":" + std::string(192, char('a' + generation));
}
static std::unique_ptr<DB> Open(const fs::path& path, const Options& options) {
    DB* db = nullptr;
    OK(DB::Open(options, path.string(), &db));
    return std::unique_ptr<DB>(db);
}

static void RunCase(const fs::path& path, size_t writers, int bloom_bits, size_t cache_bytes) {
    constexpr size_t n = 384, stable = 32, generations = 4, readers = 4;
    std::unique_ptr<Cache> cache(NewLRUCache(cache_bytes));
    Options options;
    options.writer_buffer_size = 16 * 1024; // 多次轮换，覆盖后台落盘与小缓存淘汰。
    options.block_size = 512;
    options.l0_compaction_trigger = 2;
    options.block_cache = cache.get();
    options.bloom_bits_per_key = bloom_bits;
    auto db = Open(path, options);
    for (size_t id = 0; id < n + stable; ++id) OK(db->Put(Key(id), Value(id, 0)));
    OK(db->Flush());
    // 尚未 Seek 的旧视图在并发覆盖、删除、压缩后仍须可读。
    std::unique_ptr<Iterator> old(db->NewIterator());
    std::atomic<size_t> active_writers{writers};
    RunThreads(writers + readers + 1, [&](size_t t) {
        if (t < writers) {
            try {
                // 每个键只有一个写者，最终状态与线程调度无关。
                for (size_t generation = 1; generation <= generations; ++generation)
                    for (size_t id = t; id < n; id += writers) {
                        if (generation == 2 || (generation == generations && id % 3 == 0))
                            OK(db->Delete(Key(id)));
                        else OK(db->Put(Key(id), Value(id, generation)));
                    }
            } catch (...) {
                active_writers.fetch_sub(1);
                throw;
            }
            active_writers.fetch_sub(1);
        } else if (t < writers + readers) {
            size_t attempt = 0;
            std::string value;
            std::vector<size_t> seen(n, 0);
            do {
                const size_t id = attempt % 4 == 0 ? n + (attempt + t) % stable : (attempt * 131 + t * 17) % n;
                const auto status = db->Get(Key(id), &value);
                if (id >= n) {
                    OK(status);
                    Check(value == Value(id, 0), "stable key lost or changed during concurrent writes");
                } else if (!status.IsNotFound()) {
                    OK(status);
                    bool valid = false;
                    for (size_t generation : {0, 1, 3, 4}) {
                        if (generation == generations && id % 3 == 0) continue;
                        if (value == Value(id, generation)) {
                            Check(generation >= seen[id], "sequential reads moved to an older version");
                            seen[id] = generation;
                            valid = true;
                        }
                    }
                    Check(valid, "concurrent Get returned a torn or wrong-key value");
                }
                if (++attempt % 64 == 0) cache->Prune();
            } while (active_writers.load() != 0 || attempt < 1000);
        } else {
            for (size_t id = 0; id < n + stable; ++id) {
                if (id == 0) old->SeekToFirst();
                Check(old->Valid(), "old iterator lost a key during concurrent writes");
                Check(old->key().ToString() == Key(id), "old iterator key changed");
                Check(old->value().ToString() == Value(id, 0), "old iterator view changed");
                old->Next();
            }
            Check(!old->Valid(), "old iterator returned extra keys");
            OK(old->status());
        }
    });
    OK(db->Flush());
    old.reset();
    auto verify = [&] {
        std::string value;
        for (size_t id = 0; id < n + stable; ++id) {
            const auto status = db->Get(Key(id), &value);
            if (id < n && id % 3 == 0) Check(status.IsNotFound(), "deleted key reappeared");
            else { OK(status); Check(value == Value(id, id < n ? generations : 0), "final version lost"); }
        }
        std::unique_ptr<Iterator> cursor(db->NewIterator());
        cursor->SeekToFirst();
        size_t count = 0;
        for (size_t id = 0; id < n + stable; ++id) if (id >= n || id % 3 != 0) {
            Check(cursor->Valid() && cursor->key().ToString() == Key(id), "final scan order/count mismatch");
            Check(cursor->value().ToString() == Value(id, id < n ? generations : 0), "final scan value mismatch");
            cursor->Next();
            ++count;
        }
        Check(!cursor->Valid() && count == n * 2 / 3 + stable, "final scan returned extra keys");
        OK(cursor->status());
    };
    verify();
    db.reset();
    db = Open(path, options);
    verify(); // 不仅验证内存可见性，还验证并发写后的落盘和正常重开。
}

int main() {
    const auto root = fs::temp_directory_path() / ("mapledb-concurrent-" + std::to_string(getpid()));
    try {
        // 验证线程内错误能正常报告，避免回归失败时 std::terminate。
        bool caught = false;
        try { RunThreads(4, [](size_t t) { if (t == 2) throw std::runtime_error("expected"); }); }
        catch (const std::runtime_error&) { caught = true; }
        Check(caught, "worker exception was not propagated");
        Check(fs::create_directory(root), "test directory already exists");
        for (int bloom : {0, 10})
            for (size_t writers : {1, 2, 4, 8})
                for (size_t cache : {0, 32 * 1024}) {
                    RunCase(root / (std::to_string(bloom) + "-" + std::to_string(writers) + "-" + std::to_string(cache)),
                            writers, bloom, cache);
                }
        fs::remove_all(root);
        std::cout << "16 concurrent overwrite/delete/iterator/recovery cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (data: " << root << ")\n";
        return 1;
    }
}
