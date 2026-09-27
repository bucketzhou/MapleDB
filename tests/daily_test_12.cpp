// tests/day12_test.cpp
// Day 12 测试：多线程优化
//
// 覆盖功能：
//   后台 Compaction 线程（异步 Flush + Major Compaction）
//   并发写入（多线程 Put）
//   并发读写（读线程 + 写线程同时运行）
//   写入超过 MemTable 阈值触发后台 Flush（不阻塞写入者）
//   Get() 在后台 Compaction 期间返回正确结果
//   DBImpl 析构时安全等待后台线程退出

#include "mapledb/db.h"
#include "mapledb/options.h"
#include "mapledb/status.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace mapledb;
namespace fs = std::filesystem;

// ── 测试辅助宏 ─────────────────────────────────────────────────────
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAILED: %s  at %s:%d\n", #expr, __FILE__, __LINE__);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define CHECK_OK(s) CHECK((s).Ok())
#define CHECK_NF(s) CHECK((s).IsNotFound())
#define CHECK_EQ(a, b) CHECK((a) == (b))

static const std::string kTestDir = "/tmp/minidb_day12_test";

static std::string SubDir(const std::string& name) {
    std::string d = kTestDir + "/" + name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// ════════════════════════════════════════════════════════════════
// 测试 1：并发写入 — 多线程同时调用 Put()
// 验证互斥锁正确保护写路径
// ════════════════════════════════════════════════════════════════
static void TestConcurrentWrites() {
    printf("TestConcurrentWrites...\n");

    std::string dbdir = SubDir("db_concurrent_writes");

    Options opts;
    opts.writer_buffer_size = 64 * 1024; // 小 MemTable，快速触发 Flush
    opts.create_if_missing  = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    const int kThreads       = 4;
    const int kKeysPerThread = 100;

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; t++) {
        writers.emplace_back([db, t]() {
            for (int i = 0; i < kKeysPerThread; i++) {
                char key[32], val[32];
                snprintf(key, sizeof(key), "t%d_key%04d", t, i);
                snprintf(val, sizeof(val), "t%d_val%04d", t, i);
                CHECK_OK(db->Put(std::string(key), std::string(val)));
            }
        });
    }
    for (auto& th : writers)
        th.join();

    // 验证所有 key 可以读回
    for (int t = 0; t < kThreads; t++) {
        for (int i = 0; i < kKeysPerThread; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "t%d_key%04d", t, i);
            snprintf(expected, sizeof(expected), "t%d_val%04d", t, i);
            std::string got;
            CHECK_OK(db->Get(std::string(key), &got));
            CHECK_EQ(got, std::string(expected));
        }
    }

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 2：并发读写 — 读线程和写线程同时运行
// 验证 Get() 和 Put() 可以并发执行（互斥锁保护）
// ════════════════════════════════════════════════════════════════
static void TestConcurrentReadWrite() {
    printf("TestConcurrentReadWrite...\n");

    std::string dbdir = SubDir("db_concurrent_rw");

    Options opts;
    opts.writer_buffer_size = 64 * 1024;
    opts.create_if_missing  = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 预先写入一些基础数据
    const int kBase = 50;
    for (int i = 0; i < kBase; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "base_key%04d", i);
        snprintf(val, sizeof(val), "base_val%04d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 启动写线程和读线程
    std::atomic<bool> stop{false};
    std::atomic<int>  write_count{0};
    std::atomic<int>  read_errors{0};

    // 写线程：持续写入新 key
    std::thread writer([&]() {
        for (int i = 0; i < 200; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "new_key%05d", i);
            snprintf(val, sizeof(val), "new_val%05d", i);
            Status s = db->Put(std::string(key), std::string(val));
            if (!s.Ok()) {
                read_errors.fetch_add(1);
                break;
            }
            write_count.fetch_add(1);
        }
        stop.store(true);
    });

    // 读线程：持续读基础数据（这些 key 已存在，应该总能读到）
    std::thread reader([&]() {
        while (!stop.load()) {
            for (int i = 0; i < kBase; i++) {
                char key[32];
                snprintf(key, sizeof(key), "base_key%04d", i);
                std::string got;
                Status      s = db->Get(std::string(key), &got);
                if (!s.Ok()) {
                    read_errors.fetch_add(1);
                    return;
                }
            }
        }
    });

    writer.join();
    reader.join();

    CHECK_EQ(read_errors.load(), 0);
    CHECK(write_count.load() > 0);

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 3：后台 Flush 触发 — 写入超过 MemTable 阈值
// 验证后台线程自动接管 Flush，写入者不需要同步等待
// ════════════════════════════════════════════════════════════════
static void TestBackgroundFlush() {
    printf("TestBackgroundFlush...\n");

    std::string dbdir = SubDir("db_bg_flush");

    Options opts;
    opts.writer_buffer_size = 32 * 1024; // 32KB 快速触发 Flush
    opts.create_if_missing  = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写入足够多的数据，触发多次 Flush
    const int kKeys = 500;
    for (int i = 0; i < kKeys; i++) {
        char key[32], val[64];
        snprintf(key, sizeof(key), "flush_key%05d", i);
        // 较长的 value 加速 MemTable 填满
        snprintf(val, sizeof(val), "flush_value_padding_data_%05d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 等待后台线程完成所有 Flush（通过再次写入一个 key 触发 MakeRoomForWrite）
    CHECK_OK(db->Put("sentinel", "done"));

    // 验证所有数据正确
    for (int i = 0; i < kKeys; i++) {
        char key[32], expected[64];
        snprintf(key, sizeof(key), "flush_key%05d", i);
        snprintf(expected, sizeof(expected), "flush_value_padding_data_%05d", i);
        std::string got;
        CHECK_OK(db->Get(std::string(key), &got));
        CHECK_EQ(got, std::string(expected));
    }

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 4：Get() 在后台 Compaction 期间返回正确结果
// 验证读路径与后台 Compaction 的并发安全性
// ════════════════════════════════════════════════════════════════
static void TestGetDuringCompaction() {
    printf("TestGetDuringCompaction...\n");

    std::string dbdir = SubDir("db_get_during_compaction");

    Options opts;
    opts.writer_buffer_size    = 32 * 1024;
    opts.l0_compaction_trigger = 2; // 低阈值，快速触发 Major Compaction
    opts.create_if_missing     = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 批量写入，触发 Flush + Major Compaction
    const int kRounds    = 3;
    const int kKeysRound = 100;
    for (int r = 0; r < kRounds; r++) {
        for (int i = 0; i < kKeysRound; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "r%d_key%04d", r, i);
            snprintf(val, sizeof(val), "r%d_val%04d", r, i);
            CHECK_OK(db->Put(std::string(key), std::string(val)));
        }
    }

    // 并发读写
    std::atomic<int> errors{0};

    std::thread reader([&]() {
        for (int i = 0; i < 50; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "r0_key%04d", i);
            snprintf(expected, sizeof(expected), "r0_val%04d", i);
            std::string got;
            Status      s = db->Get(std::string(key), &got);
            if (!s.Ok() || got != std::string(expected)) {
                errors.fetch_add(1);
            }
        }
    });

    std::thread writer([&]() {
        for (int i = 0; i < 50; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "extra_key%04d", i);
            snprintf(val, sizeof(val), "extra_val%04d", i);
            Status s = db->Put(std::string(key), std::string(val));
            if (!s.Ok())
                errors.fetch_add(1);
        }
    });

    reader.join();
    writer.join();

    CHECK_EQ(errors.load(), 0);

    // 最终验证数据完整性
    for (int r = 0; r < kRounds; r++) {
        for (int i = 0; i < kKeysRound; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "r%d_key%04d", r, i);
            snprintf(expected, sizeof(expected), "r%d_val%04d", r, i);
            std::string got;
            CHECK_OK(db->Get(std::string(key), &got));
            CHECK_EQ(got, std::string(expected));
        }
    }

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 5：析构安全 — 关闭 DB 时等待后台线程退出
// 验证 DBImpl 析构不会在后台线程运行时销毁资源
// ════════════════════════════════════════════════════════════════
static void TestSafeShutdown() {
    printf("TestSafeShutdown...\n");

    std::string dbdir = SubDir("db_safe_shutdown");

    Options opts;
    opts.writer_buffer_size = 16 * 1024; // 非常小，频繁 Flush
    opts.create_if_missing  = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 快速写入大量数据，确保后台线程有工作在做
    for (int i = 0; i < 300; i++) {
        char key[32], val[64];
        snprintf(key, sizeof(key), "shutdown_key%05d", i);
        snprintf(val, sizeof(val), "shutdown_value_with_padding_%05d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 直接关闭 DB — 析构函数必须安全等待后台线程完成
    delete db;
    db = nullptr;

    // 重新打开，验证数据完整性（说明关闭时数据已正确持久化）
    opts.create_if_missing = false;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 至少能读到部分已写入的数据
    std::string got;
    // key000 一定写入了（写入在 Flush 之前）
    // 注意：未 Flush 的数据通过 WAL 恢复
    CHECK_OK(db->Get("shutdown_key00000", &got));
    CHECK_EQ(got, std::string("shutdown_value_with_padding_00000"));

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 6：Major Compaction 后数据一致性
// 触发 L0→L1 Compaction，验证 Compaction 不破坏数据
// ════════════════════════════════════════════════════════════════
static void TestMajorCompactionCorrectness() {
    printf("TestMajorCompactionCorrectness...\n");

    std::string dbdir = SubDir("db_major_compaction");

    Options opts;
    opts.writer_buffer_size    = 32 * 1024;
    opts.l0_compaction_trigger = 2;
    opts.create_if_missing     = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 第一轮：写入 key0~key299
    for (int i = 0; i < 300; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(val, sizeof(val), "val_v1_%05d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 第二轮：覆盖写入 key0~key299（新版本）
    for (int i = 0; i < 300; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(val, sizeof(val), "val_v2_%05d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 等待后台 Compaction 完成（通过写入一个 sentinel 触发等待）
    CHECK_OK(db->Put("zzz_sentinel", "done"));

    // 验证读到的是最新版本
    for (int i = 0; i < 300; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(expected, sizeof(expected), "val_v2_%05d", i);
        std::string got;
        CHECK_OK(db->Get(std::string(key), &got));
        CHECK_EQ(got, std::string(expected));
    }

    // 验证 Delete 正确工作（通过 Compaction 后 tombstone 生效）
    CHECK_OK(db->Delete("key00100"));
    std::string got;
    CHECK_NF(db->Get("key00100", &got));

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 7：多线程并发写 + 并发读（压力测试）
// ════════════════════════════════════════════════════════════════
static void TestStressWriteRead() {
    printf("TestStressWriteRead...\n");

    std::string dbdir = SubDir("db_stress");

    Options opts;
    opts.writer_buffer_size    = 32 * 1024;
    opts.l0_compaction_trigger = 4;
    opts.create_if_missing     = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    const int kWriteThreads = 3;
    const int kReadThreads  = 2;
    const int kOpsPerThread = 80;

    std::atomic<int> errors{0};

    // 先写入一批基础数据供读线程使用
    for (int i = 0; i < 50; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "stable%04d", i);
        snprintf(val, sizeof(val), "stval%04d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 启动写线程
    std::vector<std::thread> write_threads;
    for (int t = 0; t < kWriteThreads; t++) {
        write_threads.emplace_back([db, t, &errors]() {
            for (int i = 0; i < kOpsPerThread; i++) {
                char key[32], val[32];
                snprintf(key, sizeof(key), "wt%d_%04d", t, i);
                snprintf(val, sizeof(val), "wv%d_%04d", t, i);
                if (!db->Put(std::string(key), std::string(val)).Ok()) {
                    errors.fetch_add(1);
                }
            }
        });
    }

    // 启动读线程（读基础数据，应始终可见）
    std::vector<std::thread> read_threads;
    for (int t = 0; t < kReadThreads; t++) {
        read_threads.emplace_back([db, &errors]() {
            for (int i = 0; i < 50; i++) {
                char key[32], expected[32];
                snprintf(key, sizeof(key), "stable%04d", i);
                snprintf(expected, sizeof(expected), "stval%04d", i);
                std::string got;
                Status      s = db->Get(std::string(key), &got);
                if (!s.Ok() || got != std::string(expected)) {
                    errors.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : write_threads)
        t.join();
    for (auto& t : read_threads)
        t.join();

    CHECK_EQ(errors.load(), 0);

    // 验证写入的数据完整
    for (int t = 0; t < kWriteThreads; t++) {
        for (int i = 0; i < kOpsPerThread; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "wt%d_%04d", t, i);
            snprintf(expected, sizeof(expected), "wv%d_%04d", t, i);
            std::string got;
            CHECK_OK(db->Get(std::string(key), &got));
            CHECK_EQ(got, std::string(expected));
        }
    }

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════════════
int main() {
    printf("===== Day 12: Multi-threading Tests =====\n\n");

    TestConcurrentWrites();
    TestConcurrentReadWrite();
    TestBackgroundFlush();
    TestGetDuringCompaction();
    TestSafeShutdown();
    TestMajorCompactionCorrectness();
    TestStressWriteRead();

    printf("\n===== All Day 12 tests PASSED =====\n");
    return 0;
}
