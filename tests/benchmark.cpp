// 性能基准测试
//
// 基准项目：
//   1. 顺序写入吞吐量   (Sequential Write)
//   2. 随机写入吞吐量   (Random Write)
//   3. 顺序读取吞吐量   (Sequential Read)
//   4. 随机读取吞吐量   (Random Read)
//   5. 顺序扫描吞吐量   (Sequential Scan via Iterator)
//
// 使用方法：
//   ./tests/day14_bench [num_keys]   默认 100000 条

#include "mapledb/db.h"
#include "mapledb/iterator.h"
#include "mapledb/options.h"
#include "mapledb/status.h"
#include "util/cache.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace fs                            = std::filesystem;
static constexpr size_t kBlockCacheSize = 8 * 1024 * 1024; // 每个 DB 使用 8MB BlockCache
// ─────────────────────────────────────────────────────────
// 工具：高精度计时器
// ─────────────────────────────────────────────────────────
struct Timer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_;

    void Start() { start_ = Clock::now(); }

    double ElapsedMs() const {
        auto now = Clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
};

// 打印吞吐量统计
static void PrintThroughput(const char* name, int n, double elapsed_ms) {
    double elapsed_s = elapsed_ms / 1000.0;
    double ops_per_s = n / elapsed_s;
    double mb_per_s  = (ops_per_s * (17 + 17)) / (1024.0 * 1024.0);
    fprintf(stdout, "  %-28s : %8d ops  %8.1f ms  %10.0f ops/s  %.2f MB/s\n", name, n, elapsed_ms,
            ops_per_s, mb_per_s);
}

// ─────────────────────────────────────────────────────────
// 生成 key / value
// ─────────────────────────────────────────────────────────
static std::string MakeKey(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "benchkey%09d", i);
    return buf;
}

static std::string MakeVal(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "benchval%09d", i);
    return buf;
}

static void RequireOK(const mapledb::Status& s) {
    if (!s.Ok()) {
        std::fprintf(stderr, "%s\n", s.ToString().c_str());
        std::exit(1);
    }
}

// ─────────────────────────────────────────────────────────
// 基准 1: 顺序写入
// ─────────────────────────────────────────────────────────
static void BenchSeqWrite(const std::string& dbpath, int N) {
    fs::remove_all(dbpath);
    mapledb::Options                opts;
    std::unique_ptr<mapledb::Cache> block_cache(mapledb::NewLRUCache(kBlockCacheSize));
    opts.writer_buffer_size = 4 * 1024 * 1024;   // 4MB
    opts.block_cache        = block_cache.get(); // DB 关闭后自动释放缓存
    mapledb::DB* db         = nullptr;
    RequireOK(mapledb::DB::Open(opts, dbpath, &db));

    Timer t;
    t.Start();
    for (int i = 0; i < N; i++) {
        RequireOK(db->Put(MakeKey(i), MakeVal(i)));
    }
    double elapsed = t.ElapsedMs();

    PrintThroughput("Sequential Write", N, elapsed);

    delete db;
}

// ─────────────────────────────────────────────────────────
// 基准 2: 随机写入
// ─────────────────────────────────────────────────────────
static void BenchRandWrite(const std::string& dbpath, int N) {
    fs::remove_all(dbpath);
    std::unique_ptr<mapledb::Cache> block_cache(mapledb::NewLRUCache(kBlockCacheSize));
    mapledb::Options                opts;
    opts.writer_buffer_size = 4 * 1024 * 1024;
    opts.block_cache        = block_cache.get();
    mapledb::DB* db         = nullptr;
    RequireOK(mapledb::DB::Open(opts, dbpath, &db));

    std::vector<int> order(N);
    for (int i = 0; i < N; i++)
        order[i] = i;
    std::mt19937 rng(42);
    std::shuffle(order.begin(), order.end(), rng);

    Timer t;
    t.Start();
    for (int i : order) {
        RequireOK(db->Put(MakeKey(i), MakeVal(i)));
    }
    double elapsed = t.ElapsedMs();

    PrintThroughput("Random  Write", N, elapsed);

    delete db;
}

// ─────────────────────────────────────────────────────────
// 基准 3 & 4: 顺序读 + 随机读
// 先写入 N 条，再统一测 Get
// ─────────────────────────────────────────────────────────
static void BenchReads(const std::string& dbpath, int N) {
    // 写入阶段（不计入基准时间）
    fs::remove_all(dbpath);
    std::unique_ptr<mapledb::Cache> block_cache(mapledb::NewLRUCache(kBlockCacheSize));
    mapledb::Options                opts;
    opts.writer_buffer_size = 4 * 1024 * 1024;
    opts.block_cache        = block_cache.get();
    mapledb::DB* db         = nullptr;
    RequireOK(mapledb::DB::Open(opts, dbpath, &db));

    for (int i = 0; i < N; i++) {
        RequireOK(db->Put(MakeKey(i), MakeVal(i)));
    }

    RequireOK(db->Flush()); // 确保 MemTable 和后台任务均已完成

    // ── 顺序读 ──
    {
        Timer t;
        t.Start();
        std::string val;
        for (int i = 0; i < N; i++) {
            RequireOK(db->Get(MakeKey(i), &val));
        }
        double elapsed = t.ElapsedMs();
        PrintThroughput("Sequential Read", N, elapsed);
    }

    // ── 随机读 ──
    {
        std::vector<int> order(N);
        for (int i = 0; i < N; i++)
            order[i] = i;
        std::mt19937 rng(42);
        std::shuffle(order.begin(), order.end(), rng);

        Timer t;
        t.Start();
        std::string val;
        for (int i : order) {
            RequireOK(db->Get(MakeKey(i), &val));
        }
        double elapsed = t.ElapsedMs();
        PrintThroughput("Random  Read", N, elapsed);
    }

    delete db;
}

// ─────────────────────────────────────────────────────────
// 基准 5: 顺序扫描（Iterator）
// ─────────────────────────────────────────────────────────
static void BenchSeqScan(const std::string& dbpath, int N) {
    // 复用上一步写入的数据（路径相同）
    std::unique_ptr<mapledb::Cache> block_cache(mapledb::NewLRUCache(kBlockCacheSize));
    mapledb::Options                opts;
    opts.block_cache = block_cache.get();
    mapledb::DB* db  = nullptr;
    RequireOK(mapledb::DB::Open(opts, dbpath, &db));

    Timer t;
    t.Start();
    auto* it    = db->NewIterator();
    int   count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        (void) it->key();
        (void) it->value();
        count++;
    }
    RequireOK(it->status());
    if (count != N) std::exit(1);
    delete it;
    double elapsed = t.ElapsedMs();

    PrintThroughput("Sequential Scan (Iterator)", count, elapsed);

    delete db;
    fs::remove_all(dbpath);
}

// ─────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    int N = 100000;
    if (argc >= 2) {
        N = std::atoi(argv[1]);
        if (N <= 0)
            N = 100000;
    }

    fprintf(stdout, "=== minidb Day14 性能基准 ===\n");
    fprintf(stdout, "    key 大小 : 17 bytes   val 大小 : 17 bytes\n");
    fprintf(stdout, "    BlockCache : %zu MB / DB\n", kBlockCacheSize / (1024 * 1024));
    fprintf(stdout, "    写入条数 : %d\n\n", N);

    const std::string dbpath_write_seq  = "/tmp/minidb_bench_seqwrite";
    const std::string dbpath_write_rand = "/tmp/minidb_bench_randwrite";
    const std::string dbpath_read       = "/tmp/minidb_bench_read";

    BenchSeqWrite(dbpath_write_seq, N);
    BenchRandWrite(dbpath_write_rand, N);
    BenchReads(dbpath_read, N);
    BenchSeqScan(dbpath_read, N);

    fprintf(stdout, "\n=== 基准测试完成 ===\n");
    return 0;
}
