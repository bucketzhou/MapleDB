#pragma once

// 两种 benchmark 共用引擎配置与数据生成器，避免并发模式改变比较口径。
#include "mapledb/db.h"
#include "util/cache.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/iterator.h"
#include "db/db_impl.h" // Pinned LevelDB 1.23: flush without forcing full-tree compaction.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr size_t kWriteBuffer = 4 * 1024 * 1024;
constexpr size_t kBlockSize = 4096;
constexpr size_t kFileSize = 2 * 1024 * 1024;
constexpr size_t kLatencyStride = 257;

static double Milliseconds(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

static void Require(const mapledb::Status& s) {
    if (!s.Ok()) throw std::runtime_error(s.ToString());
}
static void Require(const leveldb::Status& s) {
    if (!s.ok()) throw std::runtime_error(s.ToString());
}

// Counts queued + running jobs; the timed write path remains the upstream implementation.
class DrainEnv final : public leveldb::EnvWrapper {
public:
    DrainEnv() : EnvWrapper(leveldb::Env::Default()) {}
    ~DrainEnv() override { WaitIdle(); }
    void Schedule(void (*function)(void*), void* arg) override {
        auto* task = new Task{this, function, arg};
        { std::lock_guard<std::mutex> lock(mutex_); ++pending_; }
        target()->Schedule([](void* p) {
            std::unique_ptr<Task> task(static_cast<Task*>(p));
            task->function(task->arg);
            std::lock_guard<std::mutex> lock(task->env->mutex_);
            --task->env->pending_;
            task->env->cv_.notify_all();
        }, task);
    }
    void WaitIdle() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return pending_ == 0; });
    }
private:
    struct Task { DrainEnv* env; void (*function)(void*); void* arg; };
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t pending_ = 0;
};

struct MapleCursor {
    std::unique_ptr<mapledb::Iterator> iterator;
    void First() { iterator->SeekToFirst(); }
    void Seek(const std::string& key) { iterator->Seek(key); }
    bool Valid() const { return iterator->Valid(); }
    void Next() { iterator->Next(); }
    std::string_view Key() const { auto& s = iterator->key(); return {s.Data(), s.Size()}; }
    std::string_view Value() const { auto& s = iterator->value(); return {s.Data(), s.Size()}; }
    void Check() const { Require(iterator->status()); }
};
struct LevelCursor {
    std::unique_ptr<leveldb::Iterator> iterator;
    void First() { iterator->SeekToFirst(); }
    void Seek(const std::string& key) { iterator->Seek(key); }
    bool Valid() const { return iterator->Valid(); }
    void Next() { iterator->Next(); }
    std::string_view Key() const { auto s = iterator->key(); return {s.data(), s.size()}; }
    std::string_view Value() const { auto s = iterator->value(); return {s.data(), s.size()}; }
    void Check() const { Require(iterator->status()); }
};

class MapleEngine {
public:
    MapleEngine(const fs::path& path, size_t cache_bytes, int bloom_bits)
        : cache_(mapledb::NewLRUCache(cache_bytes)) {
        mapledb::Options opts;
        opts.writer_buffer_size = kWriteBuffer;
        opts.block_size = kBlockSize;
        opts.max_file_size = kFileSize;
        opts.block_restart_interval = 16;
        opts.block_cache = cache_.get();
        opts.bloom_bits_per_key = bloom_bits; // 基线默认关闭；与 LevelDB 的开关一致。
        mapledb::DB* db = nullptr;
        Require(mapledb::DB::Open(opts, path.string(), &db));
        db_.reset(db);
    }
    void Put(const std::string& k, const std::string& v) { Require(db_->Put(k, v)); }
    bool Get(const std::string& k, std::string* v) {
        auto s = db_->Get(k, v);
        if (s.IsNotFound()) return false;
        Require(s); return true;
    }
    void Drain() { Require(db_->Flush()); }
    MapleCursor NewCursor() { return {std::unique_ptr<mapledb::Iterator>(db_->NewIterator())}; }
private:
    std::unique_ptr<mapledb::Cache> cache_;
    std::unique_ptr<mapledb::DB> db_;
};

class LevelEngine {
public:
    LevelEngine(const fs::path& path, size_t cache_bytes, int bloom_bits)
        : cache_(leveldb::NewLRUCache(cache_bytes)),
          filter_(bloom_bits > 0 ? leveldb::NewBloomFilterPolicy(bloom_bits) : nullptr) {
        leveldb::Options opts;
        opts.create_if_missing = true;
        opts.write_buffer_size = kWriteBuffer;
        opts.block_size = kBlockSize;
        opts.max_file_size = kFileSize;
        opts.block_restart_interval = 16;
        opts.block_cache = cache_.get();
        opts.max_open_files = 110; // 100 TableCache entries + 10 non-table files.
        opts.compression = leveldb::kNoCompression;
        opts.filter_policy = filter_.get();
        opts.env = &env_;
        leveldb::DB* db = nullptr;
        Require(leveldb::DB::Open(opts, path.string(), &db));
        db_.reset(db);
    }
    void Put(const std::string& k, const std::string& v) {
        Require(db_->Put(leveldb::WriteOptions(), k, v)); // sync=false, batch size=1.
    }
    bool Get(const std::string& k, std::string* v) {
        auto s = db_->Get(leveldb::ReadOptions(), k, v);
        if (s.IsNotFound()) return false;
        Require(s); return true;
    }
    void Drain() {
        Require(static_cast<leveldb::DBImpl*>(db_.get())->TEST_CompactMemTable());
        env_.WaitIdle();
    }
    LevelCursor NewCursor() { return {std::unique_ptr<leveldb::Iterator>(db_->NewIterator(leveldb::ReadOptions()))}; }
private:
    DrainEnv env_;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> filter_;
    std::unique_ptr<leveldb::DB> db_;
};

static uint64_t Mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static uint64_t Hash(uint64_t h, std::string_view bytes) {
    for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static uint64_t OrderHash(const std::vector<size_t>& order) {
    uint64_t h = 14695981039346656037ULL;
    for (uint64_t i : order) for (int b = 0; b < 8; ++b) {
        h ^= static_cast<unsigned char>(i >> (8 * b)); h *= 1099511628211ULL;
    }
    return h;
}
static std::string MakeKey(uint64_t i) {
    char b[32];
    snprintf(b, sizeof b, "%016llu", static_cast<unsigned long long>(i));
    return b;
}
struct Dataset {
    std::vector<std::string> keys, values, missing;
    std::vector<size_t> sequential, random, hot;
    uint64_t hash = 14695981039346656037ULL;
    uint64_t random_hash, hot_hash;
    Dataset(size_t n, size_t value_bytes, uint64_t seed)
        : keys(n), values(n), missing(n), sequential(n), random(n), hot(n) {
        for (size_t i = 0; i < n; ++i) {
            keys[i] = MakeKey(i * 2);
            missing[i] = MakeKey(i * 2 + 1); // Interleaved misses exercise SST lookup, not just range rejection.
            values[i].resize(value_bytes);
            uint64_t bits = 0;
            for (size_t b = 0; b < value_bytes; ++b) {
                if (b % 8 == 0) bits = Mix(seed ^ (i * 0x9e3779b97f4a7c15ULL) ^ b);
                values[i][b] = static_cast<char>(bits >> (8 * (b % 8)));
            }
            hash = Hash(Hash(hash, keys[i]), values[i]);
        }
        std::iota(sequential.begin(), sequential.end(), 0);
        random = sequential;
        std::mt19937_64 rng(seed);
        std::shuffle(random.begin(), random.end(), rng);
        const size_t hot_count = std::max<size_t>(1, n / 100);
        for (auto& i : hot) i = rng() % hot_count;
        random_hash = OrderHash(random);
        hot_hash = OrderHash(hot);
    }
};
