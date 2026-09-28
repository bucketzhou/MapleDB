
// 测试：MergeIterator & 范围扫描
//
// 覆盖功能：
//   1. BasicScan         - SeekToFirst + Next 全量扫描
//   2. SeekExact         - Seek 精确定位
//   3. SeekToNonExistent - Seek 到不存在的 key（定位到下一个更大的 key）
//   4. TombstoneFilter   - Delete 后的 key 不出现在迭代器中
//   5. VersionDedup      - 同一 key 多次写入，只返回最新值
//   6. EmptyDB           - 空 DB 迭代器
//   7. MixedOperations   - Put/Delete 交替，验证最终迭代结果
//   8. AfterFlush        - 数据 Flush 到 SSTable 后迭代正确
//   9. RangeQuery        - Seek + Next 实现范围查询
//  10. IteratorNextStep  - 逐步验证 Next 顺序正确性

#include "mapledb/db.h"
#include "mapledb/iterator.h"
#include "mapledb/options.h"
#include "mapledb/status.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mapledb;
// ─────────────────────────────────────────────────────────
// 测试辅助工具
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
static mapledb::DB* OpenDB(const std::string& name, Options opts = Options{}) {
    fs::remove_all(name);
    DB*    db = nullptr;
    Status s  = DB::Open(opts, name, &db);
    assert(s.Ok());
    return db;
}

// 收集迭代器全部 kv 到 vector<pair<string,string>>
static std::vector<std::pair<std::string, std::string>> CollectAll(Iterator* it) {
    std::vector<std::pair<std::string, std::string>> result;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        result.emplace_back(std::string(it->key().Data(), it->key().Size()),
                            std::string(it->value().Data(), it->value().Size()));
    }
    return result;
}

// ─────────────────────────────────────────────────────────
// Test 1: BasicScan — SeekToFirst + Next 全量扫描
// ─────────────────────────────────────────────────────────
static void TestBasicScan() {
    fprintf(stdout, "=== TestBasicScan ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_basic");

    db->Put("b", "B");
    db->Put("a", "A");
    db->Put("c", "C");

    auto* it = db->NewIterator();
    auto  kv = CollectAll(it);
    delete it;

    // 应按 user_key 升序返回
    CHECK_EQ(kv.size(), 3u);
    if (kv.size() == 3) {
        CHECK_EQ(kv[0].first, "a");
        CHECK_EQ(kv[0].second, "A");
        CHECK_EQ(kv[1].first, "b");
        CHECK_EQ(kv[1].second, "B");
        CHECK_EQ(kv[2].first, "c");
        CHECK_EQ(kv[2].second, "C");
    }

    delete db;
    fs::remove_all("/tmp/minidb_day13_basic");
}

// ─────────────────────────────────────────────────────────
// Test 2: SeekExact — Seek 精确定位存在的 key
// ─────────────────────────────────────────────────────────
static void TestSeekExact() {
    fprintf(stdout, "=== TestSeekExact ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_seek");

    for (int i = 0; i < 5; i++) {
        std::string k = "key" + std::to_string(i);
        std::string v = "val" + std::to_string(i);
        db->Put(k, v);
    }

    auto* it = db->NewIterator();

    // Seek 到 "key2"
    it->Seek(Slice("key2"));
    CHECK(it->Valid());
    if (it->Valid()) {
        CHECK_EQ(it->key().ToString(), "key2");
        CHECK_EQ(it->value().ToString(), "val2");
    }

    // 继续 Next
    it->Next();
    CHECK(it->Valid());
    if (it->Valid()) {
        CHECK_EQ(it->key().ToString(), "key3");
    }

    delete it;
    delete db;
    fs::remove_all("/tmp/minidb_day13_seek");
}

// ─────────────────────────────────────────────────────────
// Test 3: SeekToNonExistent — Seek 不存在的 key，应落到下一个更大的 key
// ─────────────────────────────────────────────────────────
static void TestSeekToNonExistent() {
    fprintf(stdout, "=== TestSeekToNonExistent ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_seekne");

    db->Put("apple", "1");
    db->Put("cherry", "3");
    db->Put("grape", "5");

    auto* it = db->NewIterator();

    // "banana" 不存在，应定位到 "cherry"
    it->Seek(Slice("banana"));
    CHECK(it->Valid());
    if (it->Valid()) {
        CHECK_EQ(it->key().ToString(), "cherry");
    }

    // "zzz" 超出所有 key，应 !Valid()
    it->Seek(Slice("zzz"));
    CHECK(!it->Valid());

    delete it;
    delete db;
    fs::remove_all("/tmp/minidb_day13_seekne");
}

// ─────────────────────────────────────────────────────────
// Test 4: TombstoneFilter — Delete 后 key 不可见
// ─────────────────────────────────────────────────────────
static void TestTombstoneFilter() {
    fprintf(stdout, "=== TestTombstoneFilter ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_tomb");

    db->Put("x", "1");
    db->Put("y", "2");
    db->Put("z", "3");
    db->Delete("y"); // tombstone

    auto* it = db->NewIterator();
    auto  kv = CollectAll(it);
    delete it;

    // "y" 应不出现
    CHECK_EQ(kv.size(), 2u);
    if (kv.size() == 2) {
        CHECK_EQ(kv[0].first, "x");
        CHECK_EQ(kv[1].first, "z");
    }

    delete db;
    fs::remove_all("/tmp/minidb_day13_tomb");
}

// ─────────────────────────────────────────────────────────
// Test 5: VersionDedup — 同一 key 多次 Put，只返回最新值
// ─────────────────────────────────────────────────────────
static void TestVersionDedup() {
    fprintf(stdout, "=== TestVersionDedup ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_dedup");

    db->Put("k", "v1");
    db->Put("k", "v2");
    db->Put("k", "v3"); // 最新版本

    auto* it = db->NewIterator();
    auto  kv = CollectAll(it);
    delete it;

    // 只应出现一次 "k"，且值为 "v3"
    CHECK_EQ(kv.size(), 1u);
    if (!kv.empty()) {
        CHECK_EQ(kv[0].first, "k");
        CHECK_EQ(kv[0].second, "v3");
    }

    delete db;
    fs::remove_all("/tmp/minidb_day13_dedup");
}

// ─────────────────────────────────────────────────────────
// Test 6: EmptyDB — 空 DB 迭代器
// ─────────────────────────────────────────────────────────
static void TestEmptyDB() {
    fprintf(stdout, "=== TestEmptyDB ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_empty");

    auto* it = db->NewIterator();
    it->SeekToFirst();
    CHECK(!it->Valid());

    it->Seek(Slice("anything"));
    CHECK(!it->Valid());

    delete it;
    delete db;
    fs::remove_all("/tmp/minidb_day13_empty");
}

// ─────────────────────────────────────────────────────────
// Test 7: MixedOperations — Put/Delete 交替，验证最终迭代结果
// ─────────────────────────────────────────────────────────
static void TestMixedOperations() {
    fprintf(stdout, "=== TestMixedOperations ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_mixed");

    db->Put("a", "1");
    db->Put("b", "2");
    db->Delete("a");
    db->Put("c", "3");
    db->Put("a", "4"); // 重新写入 "a"
    db->Delete("b");

    auto* it = db->NewIterator();
    auto  kv = CollectAll(it);
    delete it;

    // 期望：a=4，c=3（b 已删除）
    CHECK_EQ(kv.size(), 2u);
    if (kv.size() >= 2) {
        CHECK_EQ(kv[0].first, "a");
        CHECK_EQ(kv[0].second, "4");
        CHECK_EQ(kv[1].first, "c");
        CHECK_EQ(kv[1].second, "3");
    }

    delete db;
    fs::remove_all("/tmp/minidb_day13_mixed");
}

// ─────────────────────────────────────────────────────────
// Test 8: AfterFlush — Flush 后数据在 SSTable，迭代仍正确
// ─────────────────────────────────────────────────────────
static void TestAfterFlush() {
    fprintf(stdout, "=== TestAfterFlush ===\n");

    Options opts;
    opts.writer_buffer_size = 4096; // 4KB，写几百条就触发 Flush
    auto* db                = OpenDB("/tmp/minidb_day13_flush", opts);

    // 写入足够多的数据触发 Minor Compaction（Flush）
    const int N = 200;
    for (int i = 0; i < N; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%04d", i);
        snprintf(val, sizeof(val), "val%04d", i);
        db->Put(key, val);
    }

    // 等待后台 Flush 完成（简单轮询）
    // 生产代码应使用 db->Sync() 之类，教学用 sleep 模拟
    struct timespec ts = {0, 200 * 1000 * 1000}; // 200ms
    nanosleep(&ts, nullptr);

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

    CHECK_EQ(count, N);
    CHECK(ordered);

    delete db;
    fs::remove_all("/tmp/minidb_day13_flush");
}

// ─────────────────────────────────────────────────────────
// Test 9: RangeQuery — Seek + Next 实现 [lo, hi) 范围查询
// ─────────────────────────────────────────────────────────
static void TestRangeQuery() {
    fprintf(stdout, "=== TestRangeQuery ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_range");

    const char* keys[] = {"a", "b", "c", "d", "e", "f", "g"};
    for (const char* k : keys)
        db->Put(k, k);

    auto* it = db->NewIterator();

    // 查询范围 ["c", "f")
    std::vector<std::string> result;
    for (it->Seek(Slice("c")); it->Valid(); it->Next()) {
        std::string k = it->key().ToString();
        if (k >= "f")
            break;
        result.push_back(k);
    }
    delete it;

    CHECK_EQ(result.size(), 3u); // c, d, e
    if (result.size() == 3) {
        CHECK_EQ(result[0], "c");
        CHECK_EQ(result[1], "d");
        CHECK_EQ(result[2], "e");
    }

    delete db;
    fs::remove_all("/tmp/minidb_day13_range");
}

// ─────────────────────────────────────────────────────────
// Test 10: IteratorNextStep — 逐步验证 Next 顺序正确性
// ─────────────────────────────────────────────────────────
static void TestIteratorNextStep() {
    fprintf(stdout, "=== TestIteratorNextStep ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_nextstep");

    db->Put("10", "ten");
    db->Put("20", "twenty");
    db->Put("30", "thirty");

    auto* it = db->NewIterator();
    it->SeekToFirst();

    CHECK(it->Valid());
    CHECK_EQ(it->key().ToString(), "10");
    CHECK_EQ(it->value().ToString(), "ten");

    it->Next();
    CHECK(it->Valid());
    CHECK_EQ(it->key().ToString(), "20");
    CHECK_EQ(it->value().ToString(), "twenty");

    it->Next();
    CHECK(it->Valid());
    CHECK_EQ(it->key().ToString(), "30");
    CHECK_EQ(it->value().ToString(), "thirty");

    it->Next();
    CHECK(!it->Valid()); // 已耗尽

    delete it;
    delete db;
    fs::remove_all("/tmp/minidb_day13_nextstep");
}

// ─────────────────────────────────────────────────────────
// Test 11: DeleteThenPut — Delete 再 Put 同一 key，应可见
// ─────────────────────────────────────────────────────────
static void TestDeleteThenPut() {
    fprintf(stdout, "=== TestDeleteThenPut ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_dtp");

    db->Put("k", "old");
    db->Delete("k");
    db->Put("k", "new");

    auto* it = db->NewIterator();
    auto  kv = CollectAll(it);
    delete it;

    CHECK_EQ(kv.size(), 1u);
    if (!kv.empty()) {
        CHECK_EQ(kv[0].first, "k");
        CHECK_EQ(kv[0].second, "new");
    }

    delete db;
    fs::remove_all("/tmp/minidb_day13_dtp");
}

// ─────────────────────────────────────────────────────────
// Test 12: MultipleIterators — 多个迭代器相互独立
// ─────────────────────────────────────────────────────────
static void TestMultipleIterators() {
    fprintf(stdout, "=== TestMultipleIterators ===\n");
    auto* db = OpenDB("/tmp/minidb_day13_multi");

    db->Put("a", "1");
    db->Put("b", "2");
    db->Put("c", "3");

    // 创建两个迭代器，独立推进
    auto* it1 = db->NewIterator();
    auto* it2 = db->NewIterator();

    it1->SeekToFirst();
    it2->SeekToFirst();

    CHECK(it1->Valid());
    CHECK_EQ(it1->key().ToString(), "a");

    CHECK(it2->Valid());
    CHECK_EQ(it2->key().ToString(), "a");

    it1->Next(); // it1 -> "b"
    CHECK_EQ(it1->key().ToString(), "b");

    // it2 仍在 "a"
    CHECK_EQ(it2->key().ToString(), "a");

    it2->Next();
    it2->Next(); // it2 -> "c"
    CHECK_EQ(it2->key().ToString(), "c");

    delete it1;
    delete it2;
    delete db;
    fs::remove_all("/tmp/minidb_day13_multi");
}

// ─────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────
int main() {
    fprintf(stdout, "=== Day13: MergeIterator & 范围扫描 测试 ===\n\n");

    TestBasicScan();
    TestSeekExact();
    TestSeekToNonExistent();
    TestTombstoneFilter();
    TestVersionDedup();
    TestEmptyDB();
    TestMixedOperations();
    TestAfterFlush();
    TestRangeQuery();
    TestIteratorNextStep();
    TestDeleteThenPut();
    TestMultipleIterators();

    fprintf(stdout, "\n=== 结果：%d / %d 通过 ===\n", g_pass, g_total);
    return (g_pass == g_total) ? 0 : 1;
}
