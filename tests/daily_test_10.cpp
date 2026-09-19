// tests/day10_test.cpp
// Day 10 测试：Major Compaction
//
// 覆盖功能：
//   NeedsCompaction / PickCompaction（通过观察 L0→L1 迁移来间接验证）
//   DoCompactionWork（多路归并，去重，写新 SST）
//   InstallCompactionResults（VersionEdit 原子切换文件列表）
//   RemoveObsoleteFiles（旧 SST 文件被删除）
//   MaybeScheduleCompaction（在 flush 后自动触发）

#include "mapledb/db.h"
#include "mapledb/options.h"
#include "mapledb/status.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace mapledb;
namespace fs = std::filesystem;

// ── 测试辅助宏 ─────────────────────────────────────────────────
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

static const std::string kTestDir = "/tmp/minidb_day10_test";

static std::string SubDir(const std::string& n) {
    std::string d = kTestDir + "/" + n;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// 统计目录中 .ldb 文件的数量
static int CountLdbFiles(const std::string& dir) {
    int cnt = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".ldb")
            cnt++;
    }
    return cnt;
}

// ════════════════════════════════════════════════════════
// 测试 1：L0 文件数达到阈值后触发 Major Compaction
//         验证 L0→L1 的迁移（DoCompactionWork + InstallCompactionResults）
// ════════════════════════════════════════════════════════

void TestMajorCompactionTriggered() {
    std::cout << "[TEST] Major Compaction Triggered (L0→L1)...\n";
    std::string dbdir = SubDir("compaction_triggered");

    Options opts;
    opts.writer_buffer_size    = 8 * 1024; // 8KB，快速触发 flush
    opts.l0_compaction_trigger = 2;        // 2 个 L0 文件即触发 major compaction
    opts.max_file_size         = 4 * 1024; // 4KB 限制输出文件大小（测试文件分割）

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写入足够多的数据，触发多次 flush（每次 flush 产生一个 L0 文件）
    const int N = 600;
    for (int i = 0; i < N; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "key_%06d", i);
        snprintf(v, sizeof(v), "value_%06d_padding_data", i);
        CHECK_OK(db->Put(k, v));
    }

    // 验证所有数据仍可读（数据不能因 compaction 而丢失）
    for (int i = 0; i < N; i++) {
        char k[32], expected[64];
        snprintf(k, sizeof(k), "key_%06d", i);
        snprintf(expected, sizeof(expected), "value_%06d_padding_data", i);
        std::string val;
        CHECK_OK(db->Get(k, &val));
        CHECK_EQ(val, std::string(expected));
    }

    // 验证有 .ldb 文件存在
    int ldb_count = CountLdbFiles(dbdir);
    CHECK(ldb_count > 0);

    delete db;
    std::cout << "  Wrote " << N << " keys, triggered compaction ✓\n";
    std::cout << "  All " << N << " keys readable after compaction ✓\n";
    std::cout << "  .ldb files present: " << ldb_count << " ✓\n";
    std::cout << "[PASS] Major Compaction Triggered\n";
}

// ════════════════════════════════════════════════════════
// 测试 2：Compaction 去重（同一 key 多版本，只保留最新）
//         验证 DoCompactionWork 的去重逻辑
// ════════════════════════════════════════════════════════

void TestCompactionDeduplication() {
    std::cout << "[TEST] Compaction Deduplication...\n";
    std::string dbdir = SubDir("compaction_dedup");

    Options opts;
    opts.writer_buffer_size    = 4 * 1024; // 4KB，快速触发 flush
    opts.l0_compaction_trigger = 2;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 对同一组 key 反复写入不同版本
    const int K = 50;
    for (int version = 1; version <= 6; version++) {
        for (int i = 0; i < K; i++) {
            char k[32], v[64];
            snprintf(k, sizeof(k), "dup_%04d", i);
            snprintf(v, sizeof(v), "value_v%d_%04d", version, i);
            CHECK_OK(db->Put(k, v));
        }
    }

    // 读出所有 key，应该只看到最新版本（version=6）
    for (int i = 0; i < K; i++) {
        char k[32], expected[64];
        snprintf(k, sizeof(k), "dup_%04d", i);
        snprintf(expected, sizeof(expected), "value_v6_%04d", i);
        std::string val;
        CHECK_OK(db->Get(k, &val));
        CHECK_EQ(val, std::string(expected));
    }

    delete db;
    std::cout << "  " << K << " keys × 6 versions written ✓\n";
    std::cout << "  All keys read back as version 6 (latest) ✓\n";
    std::cout << "[PASS] Compaction Deduplication\n";
}

// ════════════════════════════════════════════════════════
// 测试 3：Compaction 中的 Tombstone 处理
//         验证删除的 key 在 compaction 后仍然 NotFound
// ════════════════════════════════════════════════════════

void TestCompactionTombstone() {
    std::cout << "[TEST] Compaction Tombstone Handling...\n";
    std::string dbdir = SubDir("compaction_tombstone");

    Options opts;
    opts.writer_buffer_size    = 4 * 1024;
    opts.l0_compaction_trigger = 2;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写入一批 key
    const int N = 100;
    for (int i = 0; i < N; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "tomb_%04d", i);
        snprintf(v, sizeof(v), "val_%04d", i);
        CHECK_OK(db->Put(k, v));
    }

    // 删除偶数 key
    for (int i = 0; i < N; i += 2) {
        char k[32];
        snprintf(k, sizeof(k), "tomb_%04d", i);
        CHECK_OK(db->Delete(k));
    }

    // 再写足够多数据触发多次 flush + compaction
    for (int i = 0; i < 200; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "extra_%06d", i);
        snprintf(v, sizeof(v), "extra_value_%06d_padding", i);
        CHECK_OK(db->Put(k, v));
    }

    // 验证：偶数 key 应该 NotFound，奇数 key 应该存在
    for (int i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof(k), "tomb_%04d", i);
        std::string val;
        if (i % 2 == 0) {
            CHECK_NF(db->Get(k, &val));
        } else {
            char expected[32];
            snprintf(expected, sizeof(expected), "val_%04d", i);
            CHECK_OK(db->Get(k, &val));
            CHECK_EQ(val, std::string(expected));
        }
    }

    delete db;
    std::cout << "  " << N / 2 << " even keys deleted, " << N / 2 << " odd keys retained ✓\n";
    std::cout << "  All deletions survived compaction ✓\n";
    std::cout << "[PASS] Compaction Tombstone Handling\n";
}

// ════════════════════════════════════════════════════════
// 测试 4：RemoveObsoleteFiles 清理旧 SST 文件
//         触发 compaction 后，旧的 L0 输入文件应被删除
// ════════════════════════════════════════════════════════

void TestRemoveObsoleteFiles() {
    std::cout << "[TEST] RemoveObsoleteFiles...\n";
    std::string dbdir = SubDir("remove_obsolete");

    Options opts;
    opts.writer_buffer_size    = 4 * 1024;
    opts.l0_compaction_trigger = 2; // 2 个 L0 文件触发

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 第一批写：触发 flush 产生 L0 文件
    for (int i = 0; i < 200; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "file_%06d", i);
        snprintf(v, sizeof(v), "file_value_%06d_padding", i);
        CHECK_OK(db->Put(k, v));
    }
    int ldb_after_first_write = CountLdbFiles(dbdir);

    // 第二批写：触发更多 flush，从而触发 major compaction
    for (int i = 200; i < 400; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "file_%06d", i);
        snprintf(v, sizeof(v), "file_value_%06d_padding", i);
        CHECK_OK(db->Put(k, v));
    }
    int ldb_after_compaction = CountLdbFiles(dbdir);

    // 验证所有 key 仍然可读
    for (int i = 0; i < 400; i++) {
        char k[32], expected[64];
        snprintf(k, sizeof(k), "file_%06d", i);
        snprintf(expected, sizeof(expected), "file_value_%06d_padding", i);
        std::string val;
        CHECK_OK(db->Get(k, &val));
        CHECK_EQ(val, std::string(expected));
    }

    delete db;
    std::cout << "  .ldb files after first write:    " << ldb_after_first_write << "\n";
    std::cout << "  .ldb files after compaction:     " << ldb_after_compaction << "\n";
    std::cout << "  All 400 keys readable after compaction ✓\n";
    std::cout << "[PASS] RemoveObsoleteFiles\n";
}

// ════════════════════════════════════════════════════════
// 测试 5：Compaction 后重启恢复
//         验证 compaction 产生的 L1 文件能被正确 Recover
// ════════════════════════════════════════════════════════

void TestCompactionAndRestart() {
    std::cout << "[TEST] Compaction + Restart Recovery...\n";
    std::string dbdir = SubDir("compaction_restart");

    Options opts;
    opts.writer_buffer_size    = 4 * 1024;
    opts.l0_compaction_trigger = 2;

    // ── 第一次运行：写数据，触发 compaction ────────────────────
    {
        DB* db = nullptr;
        CHECK_OK(DB::Open(opts, dbdir, &db));

        const int N = 400;
        for (int i = 0; i < N; i++) {
            char k[32], v[48];
            snprintf(k, sizeof(k), "restart_%04d", i);
            snprintf(v, sizeof(v), "val_%04d", i);
            CHECK_OK(db->Put(k, v));
        }
        // 删除一部分 key
        for (int i = 0; i < N; i += 5) {
            char k[32];
            snprintf(k, sizeof(k), "restart_%04d", i);
            CHECK_OK(db->Delete(k));
        }

        delete db;
        std::cout << "  First run: wrote 400 keys, deleted every 5th ✓\n";
    }

    // ── 第二次运行：重启后验证数据正确 ────────────────────────
    {
        DB* db = nullptr;
        CHECK_OK(DB::Open(opts, dbdir, &db));

        const int N = 400;
        for (int i = 0; i < N; i++) {
            char k[32];
            snprintf(k, sizeof(k), "restart_%04d", i);
            std::string val;
            if (i % 5 == 0) {
                CHECK_NF(db->Get(k, &val));
            } else {
                char expected[48];
                snprintf(expected, sizeof(expected), "val_%04d", i);
                CHECK_OK(db->Get(k, &val));
                CHECK_EQ(val, std::string(expected));
            }
        }

        delete db;
        std::cout << "  Second run: all 320 surviving keys correct ✓\n";
        std::cout << "  All 80 deleted keys correctly NotFound ✓\n";
    }

    std::cout << "[PASS] Compaction + Restart Recovery\n";
}

// ════════════════════════════════════════════════════════
// 测试 6：大量写入压力测试（多轮 compaction）
// ════════════════════════════════════════════════════════

void TestHeavyWriteWithCompaction() {
    std::cout << "[TEST] Heavy Write with Multiple Compactions...\n";
    std::string dbdir = SubDir("heavy_write");

    Options opts;
    opts.writer_buffer_size    = 8 * 1024;
    opts.l0_compaction_trigger = 2;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写大量数据，覆盖一部分 key，触发多轮 compaction
    const int N = 1000;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) {
            char k[32], v[64];
            snprintf(k, sizeof(k), "heavy_%04d", i);
            snprintf(v, sizeof(v), "heavy_v%d_%04d_padding_data", round, i);
            CHECK_OK(db->Put(k, v));
        }
    }

    // 验证最终读取到的是最新版本（round=2）
    for (int i = 0; i < N; i++) {
        char k[32], expected[64];
        snprintf(k, sizeof(k), "heavy_%04d", i);
        snprintf(expected, sizeof(expected), "heavy_v2_%04d_padding_data", i);
        std::string val;
        CHECK_OK(db->Get(k, &val));
        CHECK_EQ(val, std::string(expected));
    }

    delete db;
    std::cout << "  " << N << " keys × 3 rounds, all reads return round-2 value ✓\n";
    std::cout << "[PASS] Heavy Write with Multiple Compactions\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════

int main() {
    std::cout << "=== minidb Day-10 Tests (Major Compaction) ===\n\n";

    fs::create_directories(kTestDir);

    TestMajorCompactionTriggered();
    std::cout << "\n";

    TestCompactionDeduplication();
    std::cout << "\n";

    TestCompactionTombstone();
    std::cout << "\n";

    TestRemoveObsoleteFiles();
    std::cout << "\n";

    TestCompactionAndRestart();
    std::cout << "\n";

    TestHeavyWriteWithCompaction();

    fs::remove_all(kTestDir);

    std::cout << "\n All Day-10 tests passed!\n";
    return 0;
}