// tests/day11_test.cpp
// Day 11 测试：LRU Cache + BlockCache + TableCache
//
// 覆盖功能：
//   LRUCache 基本操作（Insert / Lookup / Release / Erase）
//   LRU 淘汰：总 charge 不超过 capacity
//   NewId 唯一性
//   Prune（清除未被持有的条目）
//   deleter 回调（条目淘汰时调用）
//   被持有的 Handle 不会被淘汰（in_use_ 语义）
//   TableCache 集成（DB 层 Put/Get 通过 TableCache 命中缓存）
//   BlockCache 集成（Table::ReadBlock 使用 block_cache）
//
// 注意：ShardedLRUCache 使用 16 个分片，容量均匀分配给每个分片。
//   因此测试使用足够大的 capacity（>= 16 的倍数），并通过填满整体缓存
//   来验证淘汰行为，而不依赖特定 key 的淘汰顺序。

#include "mapledb/db.h"
#include "mapledb/options.h"
#include "mapledb/status.h"
#include "util/cache.h"
#include "util/coding.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
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

static const std::string kTestDir = "/tmp/minidb_day11_test";

static std::string SubDir(const std::string& name) {
    std::string d = kTestDir + "/" + name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// 全局删除计数（各测试开始时重置）
static int g_deleted_count = 0;

static void IntDeleter(const Slice& /*key*/, void* value) {
    g_deleted_count++;
    delete static_cast<int*>(value);
}

// ── key 工具 ──────────────────────────────────────────────────────
static std::string MakeKey64(uint64_t i) {
    char buf[sizeof(uint64_t)];
    EncodeFixed64(buf, i);
    return std::string(buf, sizeof(buf));
}

// ════════════════════════════════════════════════════════════════
// 测试 1：基本操作（Insert / Lookup / Release / Erase）
// ════════════════════════════════════════════════════════════════
static void TestLRUBasic() {
    printf("TestLRUBasic...\n");
    g_deleted_count = 0;

    // 容量 1000：远大于插入数量，不触发淘汰
    Cache* cache = NewLRUCache(1000);

    std::string k1 = MakeKey64(1), k2 = MakeKey64(2), k3 = MakeKey64(3);

    // Insert
    auto* h1 = cache->Insert(Slice(k1), new int(1), 1, IntDeleter);
    auto* h2 = cache->Insert(Slice(k2), new int(2), 1, IntDeleter);
    auto* h3 = cache->Insert(Slice(k3), new int(3), 1, IntDeleter);
    CHECK_EQ(cache->TotalCharge(), 3u);

    // Lookup 命中
    auto* lh = cache->Lookup(Slice(k1));
    CHECK(lh != nullptr);
    CHECK_EQ(*static_cast<int*>(cache->Value(lh)), 1);
    cache->Release(lh);

    // Lookup 不存在的 key 返回 nullptr
    std::string k_miss = MakeKey64(99);
    CHECK(cache->Lookup(Slice(k_miss)) == nullptr);

    // Erase：h2 仍被持有，deleter 暂不调用
    cache->Erase(Slice(k2));
    CHECK_EQ(g_deleted_count, 0);
    cache->Release(h2); // 引用归零，deleter 被调用
    CHECK_EQ(g_deleted_count, 1);

    // Value：h1 指向 int(1)
    CHECK_EQ(*static_cast<int*>(cache->Value(h1)), 1);

    cache->Release(h1);
    cache->Release(h3);

    delete cache; // 析构时释放 k1、k3
    CHECK_EQ(g_deleted_count, 3);
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 2：LRU 淘汰 — TotalCharge 不超过 capacity
// ShardedLRUCache 将容量均分给 16 个分片，各分片独立淘汰。
// 插入远超 capacity 的条目后，TotalCharge 应 <= capacity。
// ════════════════════════════════════════════════════════════════
static void TestLRUEviction() {
    printf("TestLRUEviction...\n");
    g_deleted_count = 0;

    // capacity=16：每个分片容量=1
    const size_t cap   = 16;
    Cache*       cache = NewLRUCache(cap);

    // 插入 200 个 charge=1 的条目（各自 Release，使其可被淘汰）
    for (int i = 0; i < 200; i++) {
        std::string k = MakeKey64(static_cast<uint64_t>(i));
        auto*       h = cache->Insert(Slice(k), new int(i), 1, IntDeleter);
        cache->Release(h);
    }

    // 淘汰机制生效：TotalCharge 不超过 capacity
    CHECK(cache->TotalCharge() <= cap);

    // 发生过淘汰
    CHECK(g_deleted_count > 0);

    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 3：被持有的 Handle 不被淘汰（in_use_ 语义）
// 持有 Handle 时，即使容量满了，该条目也不能被淘汰。
// ════════════════════════════════════════════════════════════════
static void TestInUseNotEvicted() {
    printf("TestInUseNotEvicted...\n");
    g_deleted_count = 0;

    // 单分片等效：capacity=16，每分片1
    Cache* cache = NewLRUCache(16);

    // 插入 k_held 并持有 handle（不 Release）
    std::string k_held = MakeKey64(9999);
    auto*       held   = cache->Insert(Slice(k_held), new int(9999), 1, IntDeleter);
    // 不 Release held！

    // 插入大量其他条目，迫使每个分片发生淘汰
    for (int i = 0; i < 200; i++) {
        std::string k = MakeKey64(static_cast<uint64_t>(i));
        auto*       h = cache->Insert(Slice(k), new int(i), 1, IntDeleter);
        cache->Release(h);
    }

    // k_held 虽然在容量很小的分片里，但因为被持有，不会被淘汰
    // 验证：value 仍然有效
    CHECK_EQ(*static_cast<int*>(cache->Value(held)), 9999);

    // Lookup：k_held 仍在缓存中（in_cache=true，refs >= 2）
    auto* lh = cache->Lookup(Slice(k_held));
    CHECK(lh != nullptr);
    CHECK_EQ(*static_cast<int*>(cache->Value(lh)), 9999);
    cache->Release(lh);

    cache->Release(held); // 释放原持有引用
    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 4：NewId 唯一性
// ════════════════════════════════════════════════════════════════
static void TestNewId() {
    printf("TestNewId...\n");

    Cache*                cache = NewLRUCache(100);
    std::vector<uint64_t> ids;
    for (int i = 0; i < 32; i++) {
        ids.push_back(cache->NewId());
    }
    // 所有 ID 互不相同且单调递增
    for (int i = 0; i < (int) ids.size() - 1; i++) {
        CHECK(ids[i] < ids[i + 1]);
    }

    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 5：Prune（清除未被持有的条目）
// ════════════════════════════════════════════════════════════════
static void TestPrune() {
    printf("TestPrune...\n");
    g_deleted_count = 0;

    Cache* cache = NewLRUCache(1000);

    std::string k1 = MakeKey64(1), k2 = MakeKey64(2), k3 = MakeKey64(3);

    // k1 持有，k2/k3 释放（可淘汰）
    auto* h1 = cache->Insert(Slice(k1), new int(1), 1, IntDeleter);
    auto* h2 = cache->Insert(Slice(k2), new int(2), 1, IntDeleter);
    cache->Release(h2);
    auto* h3 = cache->Insert(Slice(k3), new int(3), 1, IntDeleter);
    cache->Release(h3);

    // Prune 清除 k2、k3，保留 k1
    cache->Prune();
    CHECK_EQ(g_deleted_count, 2);
    CHECK_EQ(cache->TotalCharge(), 1u);

    CHECK(cache->Lookup(Slice(k2)) == nullptr);
    CHECK(cache->Lookup(Slice(k3)) == nullptr);
    auto* lh = cache->Lookup(Slice(k1));
    CHECK(lh != nullptr);
    cache->Release(lh);

    cache->Release(h1);
    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 6：容量为 0（禁用缓存）
// ════════════════════════════════════════════════════════════════
static void TestZeroCapacity() {
    printf("TestZeroCapacity...\n");
    g_deleted_count = 0;

    Cache*      cache = NewLRUCache(0);
    std::string k1    = MakeKey64(1);

    auto* h = cache->Insert(Slice(k1), new int(1), 1, IntDeleter);
    CHECK(h != nullptr);
    // 不入缓存，Lookup 返回 nullptr
    CHECK(cache->Lookup(Slice(k1)) == nullptr);
    cache->Release(h);
    // 释放后 deleter 被调用
    CHECK_EQ(g_deleted_count, 1);

    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 7：Insert 相同 key 替换旧条目
// ════════════════════════════════════════════════════════════════
static void TestInsertOverwrite() {
    printf("TestInsertOverwrite...\n");
    g_deleted_count = 0;

    Cache*      cache = NewLRUCache(1000);
    std::string k1    = MakeKey64(1);

    // 第一次插入 value=100，Release 进入 lru_ 可淘汰状态
    auto* h1 = cache->Insert(Slice(k1), new int(100), 1, IntDeleter);
    cache->Release(h1);
    CHECK_EQ(g_deleted_count, 0);

    // 第二次插入相同 key，value=200
    // 旧条目（100）在 FinishErase 时减引用，因为已 Release，refs 降到 0 → deleter 调用
    auto* h2 = cache->Insert(Slice(k1), new int(200), 1, IntDeleter);
    CHECK_EQ(g_deleted_count, 1); // value=100 被删除

    // Lookup 得到新值 200
    auto* lh = cache->Lookup(Slice(k1));
    CHECK(lh != nullptr);
    CHECK_EQ(*static_cast<int*>(cache->Value(lh)), 200);
    cache->Release(lh);

    cache->Release(h2);
    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 8：BlockCache + TableCache 集成（DB 层端到端）
// ════════════════════════════════════════════════════════════════
static void TestDBWithCaches() {
    printf("TestDBWithCaches...\n");

    std::string dbdir = SubDir("db_with_cache");

    // 创建 8MB BlockCache
    Cache* block_cache = NewLRUCache(8 * 1024 * 1024);

    Options opts;
    opts.writer_buffer_size = 64 * 1024; // 小 MemTable，快速触发 flush
    opts.block_cache        = block_cache;
    opts.create_if_missing  = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写入一批数据
    const int kNumKeys = 200;
    for (int i = 0; i < kNumKeys; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(val, sizeof(val), "value%05d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 第一轮读取（填充 TableCache 和 BlockCache）
    for (int i = 0; i < kNumKeys; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(expected, sizeof(expected), "value%05d", i);
        std::string got;
        CHECK_OK(db->Get(std::string(key), &got));
        CHECK_EQ(got, std::string(expected));
    }

    // 第二轮读取（命中 BlockCache / TableCache）
    for (int i = 0; i < kNumKeys; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "key%05d", i);
        snprintf(expected, sizeof(expected), "value%05d", i);
        std::string got;
        CHECK_OK(db->Get(std::string(key), &got));
        CHECK_EQ(got, std::string(expected));
    }

    // 查询不存在的 key
    std::string got;
    CHECK_NF(db->Get("nonexistent_key_xyz", &got));

    delete db;
    delete block_cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 9：TableCache Evict 功能 + Compaction 后缓存一致性
// ════════════════════════════════════════════════════════════════
static void TestTableCacheEvict() {
    printf("TestTableCacheEvict...\n");

    std::string dbdir = SubDir("db_table_cache_evict");

    Options opts;
    opts.writer_buffer_size = 32 * 1024;
    opts.create_if_missing  = true;

    DB* db = nullptr;
    CHECK_OK(DB::Open(opts, dbdir, &db));

    // 写入足够数据以触发 flush + major compaction
    for (int i = 0; i < 500; i++) {
        char key[32], val[64];
        snprintf(key, sizeof(key), "key%06d", i);
        snprintf(val, sizeof(val), "this is a somewhat longer value %d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 读取以填充 TableCache
    for (int i = 0; i < 200; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%06d", i);
        std::string got;
        CHECK_OK(db->Get(std::string(key), &got));
    }

    // 再次写入触发新的 compaction（旧文件被 Evict 和删除）
    for (int i = 0; i < 500; i++) {
        char key[32], val[64];
        snprintf(key, sizeof(key), "key%06d", i);
        snprintf(val, sizeof(val), "updated value %d", i);
        CHECK_OK(db->Put(std::string(key), std::string(val)));
    }

    // 读取验证更新后的值（TableCache Evict 确保看到新数据）
    for (int i = 0; i < 100; i++) {
        char key[32], expected[64];
        snprintf(key, sizeof(key), "key%06d", i);
        snprintf(expected, sizeof(expected), "updated value %d", i);
        std::string got;
        CHECK_OK(db->Get(std::string(key), &got));
        CHECK_EQ(got, std::string(expected));
    }

    delete db;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// 测试 10：大 charge 条目
// 注意：ShardedLRUCache 的淘汰在分片内进行。
// 若 charge > per_shard_capacity 且 lru_ 为空，条目仍可放入缓存。
// 因此这里只验证 Insert / Lookup / Release 的正确性。
// ════════════════════════════════════════════════════════════════
static void TestLargeCharge() {
    printf("TestLargeCharge...\n");
    g_deleted_count = 0;

    Cache*      cache = NewLRUCache(160);
    std::string k1 = MakeKey64(42), k2 = MakeKey64(43);

    // 插入 charge=100 的条目
    auto* h1 = cache->Insert(Slice(k1), new int(100), 100, IntDeleter);
    CHECK(h1 != nullptr);
    CHECK(cache->TotalCharge() >= 1u);

    // 插入 charge=50 的条目
    auto* h2 = cache->Insert(Slice(k2), new int(50), 50, IntDeleter);
    CHECK(h2 != nullptr);

    // 两个条目均可查找到
    auto* lh = cache->Lookup(Slice(k1));
    CHECK(lh != nullptr);
    CHECK_EQ(*static_cast<int*>(cache->Value(lh)), 100);
    cache->Release(lh);

    lh = cache->Lookup(Slice(k2));
    CHECK(lh != nullptr);
    CHECK_EQ(*static_cast<int*>(cache->Value(lh)), 50);
    cache->Release(lh);

    cache->Release(h1);
    cache->Release(h2);
    delete cache;
    printf("  PASSED\n");
}

// ════════════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════════════
int main() {
    printf("===== Day 11: LRU Cache Tests =====\n\n");

    TestLRUBasic();
    TestLRUEviction();
    TestInUseNotEvicted();
    TestNewId();
    TestPrune();
    TestZeroCapacity();
    TestInsertOverwrite();
    TestLargeCharge();
    TestDBWithCaches();
    TestTableCacheEvict();

    printf("\n===== All Day 11 tests PASSED =====\n");
    return 0;
}