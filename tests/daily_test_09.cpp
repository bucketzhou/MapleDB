// tests/day09_test.cpp
// 第9天测试：完整的 DB 读写 + 重启恢复

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

static const std::string kTestDir = "/tmp/minidb_day09_test";
static std::string       SubDir(const std::string& n) {
    std::string d = kTestDir + "/" + n;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
static void SetupTestDir() { fs::create_directories(kTestDir); }
static void CleanupTestDir() { fs::remove_all(kTestDir); }

// ════════════════════════════════════════════════════════
// 基础读写测试
// ════════════════════════════════════════════════════════

void TestBasicPutGet() {
    std::cout << "[TEST] Basic Put / Get...\n";
    std::string dbdir = SubDir("basic");

    Options opts;
    DB*     db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // Put 几个 key
    CHECK_OK(db->Put("apple", "red"));
    CHECK_OK(db->Put("banana", "yellow"));
    CHECK_OK(db->Put("cherry", "dark_red"));

    // Get 验证
    std::string val;
    CHECK_OK(db->Get("apple", &val));
    CHECK_EQ(val, std::string("red"));

    CHECK_OK(db->Get("banana", &val));
    CHECK_EQ(val, std::string("yellow"));

    CHECK_OK(db->Get("cherry", &val));
    CHECK_EQ(val, std::string("dark_red"));

    // 查不存在的 key
    CHECK_NF(db->Get("durian", &val));

    delete db;
    std::cout << "  Put 3 keys, Get all correct ✓\n";
    std::cout << "  NotFound for missing key ✓\n";
    std::cout << "[PASS] Basic Put / Get\n";
}

void TestOverwrite() {
    std::cout << "[TEST] Overwrite...\n";
    std::string dbdir = SubDir("overwrite");

    Options opts;
    DB*     db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    CHECK_OK(db->Put("key1", "v1"));
    CHECK_OK(db->Put("key1", "v2")); // 覆盖
    CHECK_OK(db->Put("key1", "v3")); // 再次覆盖

    std::string val;
    CHECK_OK(db->Get("key1", &val));
    CHECK_EQ(val, std::string("v3")); // 应返回最新值

    delete db;
    std::cout << "  Overwrite 3 times, Get returns latest value ✓\n";
    std::cout << "[PASS] Overwrite\n";
}

void TestDelete() {
    std::cout << "[TEST] Delete...\n";
    std::string dbdir = SubDir("delete");

    Options opts;
    DB*     db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    CHECK_OK(db->Put("key1", "v1"));
    CHECK_OK(db->Put("key2", "v2"));

    // 删除 key1
    CHECK_OK(db->Delete("key1"));

    std::string val;
    // key1 应该 NotFound
    CHECK_NF(db->Get("key1", &val));
    // key2 仍然存在
    CHECK_OK(db->Get("key2", &val));
    CHECK_EQ(val, std::string("v2"));

    delete db;
    std::cout << "  Delete key1, NotFound ✓\n";
    std::cout << "  key2 still exists ✓\n";
    std::cout << "[PASS] Delete\n";
}

// ════════════════════════════════════════════════════════
// Minor Compaction 测试（触发 MemTable Flush）
// ════════════════════════════════════════════════════════

void TestMemTableFlush() {
    std::cout << "[TEST] MemTable Flush (Minor Compaction)...\n";
    std::string dbdir = SubDir("flush");

    Options opts;
    opts.writer_buffer_size = 64 * 1024; // 64KB，方便触发 Flush
    DB* db                  = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写入足够多的数据触发 Flush
    const int N = 2000;
    for (int i = 0; i < N; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "key_%06d", i);
        snprintf(v, sizeof(v), "value_%06d_padding_xxxx", i);
        CHECK_OK(db->Put(k, v));
    }

    // 验证所有数据都能读到（可能在 MemTable 或 SSTable 里）
    for (int i = 0; i < N; i++) {
        char k[32], expected_v[64];
        snprintf(k, sizeof(k), "key_%06d", i);
        snprintf(expected_v, sizeof(expected_v), "value_%06d_padding_xxxx", i);
        std::string val;
        CHECK_OK(db->Get(k, &val));
        CHECK_EQ(val, std::string(expected_v));
    }

    delete db;

    // 验证 Level 0 有 SSTable 文件生成
    bool has_ldb = false;
    for (const auto& entry : fs::directory_iterator(dbdir)) {
        if (entry.path().extension() == ".ldb") {
            has_ldb = true;
            break;
        }
    }
    CHECK(has_ldb);

    std::cout << "  Wrote " << N << " keys, triggered flush ✓\n";
    std::cout << "  All " << N << " keys readable after flush ✓\n";
    std::cout << "  .ldb file exists in DB dir ✓\n";
    std::cout << "[PASS] MemTable Flush\n";
}

// ════════════════════════════════════════════════════════
// 重启恢复测试（核心）
// ════════════════════════════════════════════════════════

void TestRestartRecovery() {
    std::cout << "[TEST] Restart Recovery...\n";
    std::string dbdir = SubDir("recovery");

    Options opts;

    // ── 第一次运行：写数据 ────────────────────────────────────
    {
        DB* db = nullptr;
        CHECK_OK(DB::Open(opts, dbdir, &db));

        CHECK_OK(db->Put("name", "minidb"));
        CHECK_OK(db->Put("lang", "cpp"));
        CHECK_OK(db->Put("year", "2024"));
        CHECK_OK(db->Delete("lang")); // 删除 "lang"

        delete db; // 关闭 DB（数据仍在 WAL 和可能的 SSTable 里）
        std::cout << "  First run: wrote 3 keys, deleted 1 ✓\n";
    }

    // ── 第二次运行：重启后恢复 ────────────────────────────────
    {
        DB* db = nullptr;
        CHECK_OK(DB::Open(opts, dbdir, &db));

        std::string val;

        // "name" 和 "year" 应该能读到
        CHECK_OK(db->Get("name", &val));
        CHECK_EQ(val, std::string("minidb"));

        CHECK_OK(db->Get("year", &val));
        CHECK_EQ(val, std::string("2024"));

        // "lang" 已被删除，应该 NotFound
        CHECK_NF(db->Get("lang", &val));

        delete db;
        std::cout << "  Second run: 'name' and 'year' recovered ✓\n";
        std::cout << "  'lang' correctly NotFound after deletion ✓\n";
    }

    std::cout << "[PASS] Restart Recovery\n";
}

void TestRestartWithFlush() {
    std::cout << "[TEST] Restart after Flush...\n";
    std::string dbdir = SubDir("restart_flush");

    Options opts;
    opts.writer_buffer_size = 32 * 1024; // 32KB，快速触发 Flush

    // ── 第一次运行：写足够多数据触发 Flush，然后再写一些 ────────
    {
        DB* db = nullptr;
        CHECK_OK(DB::Open(opts, dbdir, &db));

        // 批量写，触发 Flush（数据进入 SSTable）
        for (int i = 0; i < 1000; i++) {
            char k[32], v[48];
            snprintf(k, sizeof(k), "key_%04d", i);
            snprintf(v, sizeof(v), "val_%04d", i);
            CHECK_OK(db->Put(k, v));
        }
        // 再写一些（在 MemTable 里，未 Flush，通过 WAL 恢复）
        CHECK_OK(db->Put("extra_1", "after_flush_1"));
        CHECK_OK(db->Put("extra_2", "after_flush_2"));

        delete db;
        std::cout << "  First run: 1000 flushed + 2 in WAL ✓\n";
    }

    // ── 第二次运行：验证 SSTable + WAL 的数据都正确恢复 ─────────
    {
        DB* db = nullptr;
        CHECK_OK(DB::Open(opts, dbdir, &db));

        // 检查 SSTable 里的数据
        std::string val;
        CHECK_OK(db->Get("key_0000", &val));
        CHECK_EQ(val, std::string("val_0000"));

        CHECK_OK(db->Get("key_0999", &val));
        CHECK_EQ(val, std::string("val_0999"));

        // 检查 WAL 恢复的数据
        CHECK_OK(db->Get("extra_1", &val));
        CHECK_EQ(val, std::string("after_flush_1"));

        CHECK_OK(db->Get("extra_2", &val));
        CHECK_EQ(val, std::string("after_flush_2"));

        delete db;
        std::cout << "  SSTable data: key_0000 and key_0999 OK ✓\n";
        std::cout << "  WAL data: extra_1 and extra_2 OK ✓\n";
    }

    std::cout << "[PASS] Restart after Flush\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════

int main() {
    std::cout << "=== minidb Day-09 Tests ===\n\n";

    SetupTestDir();

    TestBasicPutGet();
    std::cout << "\n";

    TestOverwrite();
    std::cout << "\n";

    TestDelete();
    std::cout << "\n";

    TestMemTableFlush();
    std::cout << "\n";

    TestRestartRecovery();
    std::cout << "\n";

    TestRestartWithFlush();

    CleanupTestDir();

    std::cout << "\n All Day-09 tests passed!\n";
    return 0;
}