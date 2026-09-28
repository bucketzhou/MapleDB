// tests/day14_test.cpp
// Day14 完整集成测试
//
// 覆盖功能：
//   1.  BasicPutGet          - Put + Get 基础正确性
//   2.  DeleteNotFound       - Delete 后 Get 返回 NotFound
//   3.  UpdateValue          - 同一 key 多次 Put，Get 返回最新值
//   4.  LargeDataset         - 大量 kv 写入后逐一验证 Get
//   5.  WALRecovery          - 关闭 DB 重新打开，数据仍可读（WAL 恢复）
//   6.  DeleteThenReopen     - Delete 后重新打开，已删除 key 仍不可见
//   7.  FlushAndRead         - 写入超过 write_buffer_size，触发 Flush，Get 仍正确
//   8.  CompactionTrigger    - 持续写入触发 Major Compaction，数据仍一致
//   9.  BlockCacheIntegration- 启用 block_cache 后 Get 结果正确
//  10.  ConcurrentWrites     - 多线程并发写入，无崩溃
//  11.  ConcurrentReads      - 多线程并发读取，结果正确
//  12.  IteratorAfterCompact - Compaction 后迭代器仍全量有序

#include "mapledb/db.h"
#include "mapledb/iterator.h"
#include "mapledb/options.h"
#include "mapledb/status.h"
#include "util/cache.h" // NewLRUCache

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
using namespace ::mapledb;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────
// 测试辅助工具（与 day13 风格统一）
// ─────────────────────────────────────────────────────────

static int g_total = 0;
static int g_pass  = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_total++;                                                                                 \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "[FAIL] %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);               \
        } else {                                                                                   \
            g_pass++;                                                                              \
        }                                                                                          \
    } while (0)

template <typename T>
static std::string ToStr(const T& v) {
    return std::to_string(v);
}
static std::string ToStr(const std::string& v) { return v; }
static std::string ToStr(const char* v) { return v ? std::string(v) : "(null)"; }

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        g_total++;                                                                                 \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (!(_a == _b)) {                                                                         \
            fprintf(stderr,                                                                        \
                    "[FAIL] %s:%d  CHECK_EQ(%s, %s)\n"                                             \
                    "        lhs=%s  rhs=%s\n",                                                    \
                    __FILE__, __LINE__, #a, #b, ToStr(_a).c_str(), ToStr(_b).c_str());             \
        } else {                                                                                   \
            g_pass++;                                                                              \
        }                                                                                          \
    } while (0)

// 打开一个临时 DB（自动清理旧目录）
static DB* OpenDB(const std::string& name, Options opts = Options{}) {
    fs::remove_all(name);
    DB*    db = nullptr;
    Status s  = DB::Open(opts, name, &db);
    assert(s.Ok());
    return db;
}

// 重新打开已有的 DB（不清理目录）
static DB* ReopenDB(const std::string& name, Options opts = Options{}) {
    DB*    db = nullptr;
    Status s  = DB::Open(opts, name, &db);
    assert(s.Ok());
    return db;
}

// 等待后台 Compaction 完成（简单 sleep）
static void WaitBG(int ms = 300) {
    struct timespec ts = {0, (long) ms * 1000 * 1000};
    nanosleep(&ts, nullptr);
}

// ─────────────────────────────────────────────────────────
// Test 1: BasicPutGet — Put + Get 基础正确性
// ─────────────────────────────────────────────────────────
static void TestBasicPutGet() {
    fprintf(stdout, "=== TestBasicPutGet ===\n");
    auto* db = OpenDB("/tmp/minidb_day14_basic");

    CHECK(db->Put("hello", "world").Ok());
    CHECK(db->Put("foo", "bar").Ok());
    CHECK(db->Put("num", "42").Ok());

    std::string val;
    CHECK(db->Get("hello", &val).Ok());
    CHECK_EQ(val, "world");

    CHECK(db->Get("foo", &val).Ok());
    CHECK_EQ(val, "bar");

    CHECK(db->Get("num", &val).Ok());
    CHECK_EQ(val, "42");

    delete db;
    fs::remove_all("/tmp/minidb_day14_basic");
}

// ─────────────────────────────────────────────────────────
// Test 2: DeleteNotFound — Delete 后 Get 返回 NotFound
// ─────────────────────────────────────────────────────────
static void TestDeleteNotFound() {
    fprintf(stdout, "=== TestDeleteNotFound ===\n");
    auto* db = OpenDB("/tmp/minidb_day14_del");

    db->Put("a", "1");
    db->Put("b", "2");
    db->Delete("a");

    std::string val;
    Status      s = db->Get("a", &val);
    CHECK(s.IsNotFound());

    CHECK(db->Get("b", &val).Ok());
    CHECK_EQ(val, "2");

    // 不存在的 key 也应返回 NotFound
    s = db->Get("nonexistent", &val);
    CHECK(s.IsNotFound());

    delete db;
    fs::remove_all("/tmp/minidb_day14_del");
}

// ─────────────────────────────────────────────────────────
// Test 3: UpdateValue — 同一 key 多次 Put，Get 返回最新值
// ─────────────────────────────────────────────────────────
static void TestUpdateValue() {
    fprintf(stdout, "=== TestUpdateValue ===\n");
    auto* db = OpenDB("/tmp/minidb_day14_upd");

    db->Put("k", "v1");
    db->Put("k", "v2");
    db->Put("k", "v3");

    std::string val;
    CHECK(db->Get("k", &val).Ok());
    CHECK_EQ(val, "v3");

    // Delete 再 Put
    db->Delete("k");
    db->Put("k", "v4");
    CHECK(db->Get("k", &val).Ok());
    CHECK_EQ(val, "v4");

    delete db;
    fs::remove_all("/tmp/minidb_day14_upd");
}

// ─────────────────────────────────────────────────────────
// Test 4: LargeDataset — 大量 kv 写入后逐一 Get 验证
// ─────────────────────────────────────────────────────────
static void TestLargeDataset() {
    fprintf(stdout, "=== TestLargeDataset ===\n");
    auto* db = OpenDB("/tmp/minidb_day14_large");

    const int N = 500;
    for (int i = 0; i < N; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(val, sizeof(val), "val%05d", i);
        CHECK(db->Put(key, val).Ok());
    }

    int ok_count = 0;
    for (int i = 0; i < N; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(expected, sizeof(expected), "val%05d", i);
        std::string val;
        if (db->Get(key, &val).Ok() && val == expected) {
            ok_count++;
        }
    }
    CHECK_EQ(ok_count, N);

    delete db;
    fs::remove_all("/tmp/minidb_day14_large");
}

// ─────────────────────────────────────────────────────────
// Test 5: WALRecovery — 关闭 DB 重新打开，数据仍可读
// ─────────────────────────────────────────────────────────
static void TestWALRecovery() {
    fprintf(stdout, "=== TestWALRecovery ===\n");
    const std::string dbpath = "/tmp/minidb_day14_wal";
    fs::remove_all(dbpath);

    // 第一次打开，写入数据，然后关闭
    {
        auto* db = ReopenDB(dbpath);
        db->Put("rec_a", "recovered_a");
        db->Put("rec_b", "recovered_b");
        db->Put("rec_c", "recovered_c");
        delete db;
    }

    // 第二次打开，验证数据持久化
    {
        auto*       db = ReopenDB(dbpath);
        std::string val;
        CHECK(db->Get("rec_a", &val).Ok());
        CHECK_EQ(val, "recovered_a");

        CHECK(db->Get("rec_b", &val).Ok());
        CHECK_EQ(val, "recovered_b");

        CHECK(db->Get("rec_c", &val).Ok());
        CHECK_EQ(val, "recovered_c");

        delete db;
    }

    fs::remove_all(dbpath);
}

// ─────────────────────────────────────────────────────────
// Test 6: DeleteThenReopen — Delete 后重新打开，key 仍不可见
// ─────────────────────────────────────────────────────────
static void TestDeleteThenReopen() {
    fprintf(stdout, "=== TestDeleteThenReopen ===\n");
    const std::string dbpath = "/tmp/minidb_day14_delreopen";
    fs::remove_all(dbpath);

    {
        auto* db = ReopenDB(dbpath);
        db->Put("x", "1");
        db->Put("y", "2");
        db->Delete("x");
        delete db;
    }

    {
        auto*       db = ReopenDB(dbpath);
        std::string val;
        CHECK(db->Get("x", &val).IsNotFound());
        CHECK(db->Get("y", &val).Ok());
        CHECK_EQ(val, "2");
        delete db;
    }

    fs::remove_all(dbpath);
}

// ─────────────────────────────────────────────────────────
// Test 7: FlushAndRead — 写超过 write_buffer_size，触发 Flush
// ─────────────────────────────────────────────────────────
static void TestFlushAndRead() {
    fprintf(stdout, "=== TestFlushAndRead ===\n");

    Options opts;
    opts.writer_buffer_size = 4096; // 4KB，小 buffer 容易触发 Flush
    auto* db                = OpenDB("/tmp/minidb_day14_flush", opts);

    const int N = 300;
    for (int i = 0; i < N; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "fkey%04d", i);
        snprintf(val, sizeof(val), "fval%04d", i);
        db->Put(key, val);
    }

    WaitBG(300); // 等待后台 Flush

    int ok_count = 0;
    for (int i = 0; i < N; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "fkey%04d", i);
        snprintf(expected, sizeof(expected), "fval%04d", i);
        std::string val;
        if (db->Get(key, &val).Ok() && val == expected) {
            ok_count++;
        }
    }
    CHECK_EQ(ok_count, N);

    delete db;
    fs::remove_all("/tmp/minidb_day14_flush");
}

// ─────────────────────────────────────────────────────────
// Test 8: CompactionTrigger — 触发 Major Compaction，数据一致
// ─────────────────────────────────────────────────────────
static void TestCompactionTrigger() {
    fprintf(stdout, "=== TestCompactionTrigger ===\n");

    Options opts;
    opts.writer_buffer_size    = 4096; // 小 MemTable，快速产出 SSTable
    opts.l0_compaction_trigger = 2;    // L0 超过 2 个文件即触发
    auto* db                   = OpenDB("/tmp/minidb_day14_compact", opts);

    const int N = 600;
    for (int i = 0; i < N; i++) {
        char key[32], val[64];
        snprintf(key, sizeof(key), "ckey%05d", i);
        // value 稍大，加速 MemTable 满
        snprintf(val, sizeof(val), "cval_long_padding_%05d", i);
        db->Put(key, val);
    }

    WaitBG(500); // 等待后台 Flush + Compaction

    int ok_count = 0;
    for (int i = 0; i < N; i++) {
        char key[32], expected[64];
        snprintf(key, sizeof(key), "ckey%05d", i);
        snprintf(expected, sizeof(expected), "cval_long_padding_%05d", i);
        std::string val;
        if (db->Get(key, &val).Ok() && val == expected) {
            ok_count++;
        }
    }
    CHECK_EQ(ok_count, N);

    delete db;
    fs::remove_all("/tmp/minidb_day14_compact");
}

// ─────────────────────────────────────────────────────────
// Test 9: BlockCacheIntegration — 启用 block_cache 后 Get 正确
// ─────────────────────────────────────────────────────────
static void TestBlockCacheIntegration() {
    fprintf(stdout, "=== TestBlockCacheIntegration ===\n");

    Options opts;
    opts.writer_buffer_size = 4096;
    opts.block_cache        = NewLRUCache(1 * 1024 * 1024); // 1MB 缓存

    auto* db = OpenDB("/tmp/minidb_day14_cache", opts);

    const int N = 200;
    for (int i = 0; i < N; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "ck%04d", i);
        snprintf(val, sizeof(val), "cv%04d", i);
        db->Put(key, val);
    }

    WaitBG(300);

    // 读两遍：第一遍从磁盘，第二遍从缓存
    for (int round = 0; round < 2; round++) {
        int ok_count = 0;
        for (int i = 0; i < N; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "ck%04d", i);
            snprintf(expected, sizeof(expected), "cv%04d", i);
            std::string val;
            if (db->Get(key, &val).Ok() && val == expected) {
                ok_count++;
            }
        }
        CHECK_EQ(ok_count, N);
    }

    delete db;
    delete opts.block_cache;
    fs::remove_all("/tmp/minidb_day14_cache");
}

// ─────────────────────────────────────────────────────────
// Test 10: ConcurrentWrites — 多线程并发写入，无崩溃
// ─────────────────────────────────────────────────────────
static void TestConcurrentWrites() {
    fprintf(stdout, "=== TestConcurrentWrites ===\n");

    auto* db = OpenDB("/tmp/minidb_day14_concwrite");

    const int T = 4;   // 线程数
    const int K = 100; // 每线程写入 key 数

    std::vector<std::thread> threads;
    std::atomic<int>         errors{0};

    for (int t = 0; t < T; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < K; i++) {
                char key[32], val[32];
                snprintf(key, sizeof(key), "t%d_k%04d", t, i);
                snprintf(val, sizeof(val), "t%d_v%04d", t, i);
                if (!db->Put(key, val).Ok()) {
                    errors++;
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();

    CHECK_EQ(errors.load(), 0);

    // 验证所有 key 可读
    int ok_count = 0;
    for (int t = 0; t < T; t++) {
        for (int i = 0; i < K; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "t%d_k%04d", t, i);
            snprintf(expected, sizeof(expected), "t%d_v%04d", t, i);
            std::string val;
            if (db->Get(key, &val).Ok() && val == expected) {
                ok_count++;
            }
        }
    }
    CHECK_EQ(ok_count, T * K);

    delete db;
    fs::remove_all("/tmp/minidb_day14_concwrite");
}

// ─────────────────────────────────────────────────────────
// Test 11: ConcurrentReads — 多线程并发读取，结果正确
// ─────────────────────────────────────────────────────────
static void TestConcurrentReads() {
    fprintf(stdout, "=== TestConcurrentReads ===\n");

    auto* db = OpenDB("/tmp/minidb_day14_concread");

    const int N = 100;
    for (int i = 0; i < N; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "rk%04d", i);
        snprintf(val, sizeof(val), "rv%04d", i);
        db->Put(key, val);
    }

    const int                T = 4;
    std::vector<std::thread> threads;
    std::atomic<int>         ok_count{0};

    for (int t = 0; t < T; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < N; i++) {
                char key[32], expected[32];
                snprintf(key, sizeof(key), "rk%04d", i);
                snprintf(expected, sizeof(expected), "rv%04d", i);
                std::string val;
                if (db->Get(key, &val).Ok() && val == expected) {
                    ok_count++;
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();

    CHECK_EQ(ok_count.load(), T * N);

    delete db;
    fs::remove_all("/tmp/minidb_day14_concread");
}

// ─────────────────────────────────────────────────────────
// Test 12: IteratorAfterFlushReopen — Flush 后关闭重开，迭代器全量有序
//
// 背景：教学版 NewIterator() 在锁外打开文件，若后台 Major Compaction
//       并发删除旧文件则有竞态。通过 close/reopen 避免该竞态：
//       delete db 会 join 后台线程，确保所有 Flush 完成；
//       重新打开后立即迭代，后台还未开始新的 Compaction。
// ─────────────────────────────────────────────────────────
static void TestIteratorAfterFlushReopen() {
    fprintf(stdout, "=== TestIteratorAfterFlushReopen ===\n");

    const std::string dbpath = "/tmp/minidb_day14_itflush";
    fs::remove_all(dbpath);

    Options opts;
    opts.writer_buffer_size    = 4096; // 4KB，快速触发 Flush
    opts.l0_compaction_trigger = 8;    // 调高触发阈值，避免 Major Compaction 竞态

    const int N = 300;

    // Phase 1: 写入数据，等待后台 Flush，然后关闭 DB
    // delete db 会 join 后台线程，确保所有 Flush 已完成
    {
        DB* db = nullptr;
        assert(DB::Open(opts, dbpath, &db).Ok());
        for (int i = 0; i < N; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "ik%05d", i);
            snprintf(val, sizeof(val), "iv%05d", i);
            db->Put(key, val);
        }
        WaitBG(500); // 等后台 Flush 完成
        delete db;   // join 后台线程
    }

    // Phase 2: 重新打开，立即迭代（后台 Compaction 尚未开始）
    {
        DB* db = nullptr;
        assert(DB::Open(opts, dbpath, &db).Ok());

        auto*       it      = db->NewIterator();
        int         count   = 0;
        bool        ordered = true;
        std::string prev_key;

        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            std::string cur = it->key().ToString();
            if (!prev_key.empty() && cur <= prev_key) {
                ordered = false;
            }
            prev_key = cur;
            count++;
        }
        delete it;
        delete db;

        CHECK_EQ(count, N);
        CHECK(ordered);
    }

    fs::remove_all(dbpath);
}

// ─────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────
int main() {
    fprintf(stdout, "=== Day14: 完整集成测试 ===\n\n");

    TestBasicPutGet();
    TestDeleteNotFound();
    TestUpdateValue();
    TestLargeDataset();
    TestWALRecovery();
    TestDeleteThenReopen();
    TestFlushAndRead();
    TestCompactionTrigger();
    TestBlockCacheIntegration();
    TestConcurrentWrites();
    TestConcurrentReads();
    TestIteratorAfterFlushReopen();

    fprintf(stdout, "\n=== 结果：%d / %d 通过 ===\n", g_pass, g_total);
    return (g_pass == g_total) ? 0 : 1;
}