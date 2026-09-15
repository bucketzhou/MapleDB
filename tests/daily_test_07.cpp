// tests/day07_test.cpp
// 第7天测试：Block 读取 + Table 读取 + BloomFilter

#include "mapledb/options.h"
#include "table/block.h"
#include "table/blockbuilder.h"
#include "table/format.h"
#include "table/table.h"
#include "table/tablebuilder.h"
#include "util/bloom.h"

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
#define CHECK_EQ(a, b) CHECK((a) == (b))

static const std::string kTestDir = "/tmp/minidb_day07_test";
static void              SetupTestDir() { fs::create_directories(kTestDir); }
static void              CleanupTestDir() { fs::remove_all(kTestDir); }
static std::string       F(const std::string& n) { return kTestDir + "/" + n; }

// ════════════════════════════════════════════════════════
// Block 读取测试
// ════════════════════════════════════════════════════════

void TestBlockIteratorBasic() {
    std::cout << "[TEST] Block Iterator Basic...\n";

    BlockBuilder builder(4);
    builder.Add("apple", "v_apple");
    builder.Add("banana", "v_banana");
    builder.Add("cherry", "v_cherry");
    builder.Add("date", "v_date");
    builder.Add("elderberry", "v_elderberry"); // restart point（第5个，interval=4）
    builder.Add("fig", "v_fig");

    Slice block_data = builder.Finish();
    Block block(block_data.Data(), block_data.Size(), false);

    // SeekToFirst 遍历
    Block::Iter* iter = block.NewIterator(BytewiseComparator());
    iter->SeekToFirst();
    std::vector<std::string> keys_got;
    while (iter->Valid()) {
        keys_got.push_back(iter->key().ToString());
        iter->Next();
    }
    CHECK(iter->status().Ok());

    std::vector<std::string> expected = {"apple", "banana", "cherry", "date", "elderberry", "fig"};
    CHECK_EQ(keys_got.size(), expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
        CHECK_EQ(keys_got[i], expected[i]);
    }

    delete iter;
    std::cout << "  Forward traversal: 6 keys ✓\n";
    std::cout << "[PASS] Block Iterator Basic\n";
}

void TestBlockIteratorSeek() {
    std::cout << "[TEST] Block Iterator Seek...\n";

    // 插入偶数 key：aa, bb, cc, dd, ee, ff, gg, hh（8个）
    BlockBuilder             builder(4); // restart at aa, ee
    std::vector<std::string> ks = {"aa", "bb", "cc", "dd", "ee", "ff", "gg", "hh"};
    for (auto& k : ks)
        builder.Add(k, "v_" + k);
    Slice block_data = builder.Finish();
    Block block(block_data.Data(), block_data.Size(), false);

    Block::Iter* iter = block.NewIterator(BytewiseComparator());

    // Seek 到已存在的 key
    iter->Seek(Slice("cc"));
    CHECK(iter->Valid());
    CHECK_EQ(iter->key().ToString(), std::string("cc"));
    CHECK_EQ(iter->value().ToString(), std::string("v_cc"));

    // Seek 到不存在的 key（应落在下一个）
    iter->Seek(Slice("bc")); // "bc" 不存在，落在 "cc"
    CHECK(iter->Valid());
    CHECK_EQ(iter->key().ToString(), std::string("cc"));

    // Seek 到比所有 key 都小
    iter->Seek(Slice("a"));
    CHECK(iter->Valid());
    CHECK_EQ(iter->key().ToString(), std::string("aa"));

    // Seek 到比所有 key 都大
    iter->Seek(Slice("zz"));
    CHECK(!iter->Valid());

    // SeekToLast
    iter->SeekToLast();
    CHECK(iter->Valid());
    CHECK_EQ(iter->key().ToString(), std::string("hh"));

    delete iter;
    std::cout << "  Seek tests passed ✓\n";
    std::cout << "[PASS] Block Iterator Seek\n";
}

// ════════════════════════════════════════════════════════
// Table 读写联合测试（Write Day6 + Read Day7）
// ════════════════════════════════════════════════════════

void TestTableOpenAndGet() {
    std::cout << "[TEST] Table Open + Get...\n";

    std::string path = F("test_get.ldb");

    // 先用 TableBuilder 写入
    Options opts;
    opts.block_size = 256; // 小 block，触发多个 DataBlock

    const int                N = 100;
    std::vector<std::string> test_keys;
    {
        TableBuilder tb(path, opts);
        for (int i = 0; i < N; i++) {
            char k[32], v[32];
            snprintf(k, sizeof(k), "key_%04d", i);
            snprintf(v, sizeof(v), "val_%04d", i);
            tb.Add(Slice(k), Slice(v));
            test_keys.push_back(k);
        }
        CHECK(tb.Finish().Ok());
    }

    // 用 Table::Open 读取
    Table* table = nullptr;
    Status s     = Table::Open(path, opts, &table);
    CHECK(s.Ok());
    CHECK(table != nullptr);

    // 逐个查找，验证 value 正确
    for (int i = 0; i < N; i++) {
        char expected_k[32], expected_v[32];
        snprintf(expected_k, sizeof(expected_k), "key_%04d", i);
        snprintf(expected_v, sizeof(expected_v), "val_%04d", i);

        struct Result {
            bool        found = false;
            std::string key, value;
        } result;

        s = table->InternalGet(Slice(expected_k), &result,
                               [](void* arg, const Slice& k, const Slice& v) {
                                   auto* r  = static_cast<Result*>(arg);
                                   r->found = true;
                                   r->key   = k.ToString();
                                   r->value = v.ToString();
                               });
        CHECK(s.Ok());
        CHECK(result.found);
        CHECK_EQ(result.value, std::string(expected_v));
    }

    // 查找不存在的 key
    struct Result {
        bool found = false;
    } nr;
    s = table->InternalGet(Slice("zzzz_notexist"), &nr, [](void* arg, const Slice&, const Slice&) {
        static_cast<Result*>(arg)->found = true;
    });
    CHECK(s.Ok());
    CHECK(!nr.found);

    delete table;
    std::cout << "  " << N << " keys: all Get correct ✓\n";
    std::cout << "  Non-existent key: not found ✓\n";
    std::cout << "[PASS] Table Open + Get\n";
}

void TestTableIterator() {
    std::cout << "[TEST] Table Iterator (full scan)...\n";

    std::string path = F("test_iter.ldb");
    Options     opts;
    opts.block_size = 256;

    const int N = 80;
    {
        TableBuilder tb(path, opts);
        for (int i = 0; i < N; i++) {
            char k[32], v[32];
            snprintf(k, sizeof(k), "key_%04d", i);
            snprintf(v, sizeof(v), "val_%04d", i);
            tb.Add(Slice(k), Slice(v));
        }
        CHECK(tb.Finish().Ok());
    }

    Table* table = nullptr;
    CHECK(Table::Open(path, opts, &table).Ok());

    // 全扫描，验证顺序和数量
    Table::Iterator* iter = table->NewIterator();
    iter->SeekToFirst();

    int         count = 0;
    std::string prev_key;
    while (iter->Valid()) {
        std::string cur_key = iter->key().ToString();
        if (!prev_key.empty()) {
            CHECK(cur_key > prev_key); // 严格递增
        }
        prev_key = cur_key;
        count++;
        iter->Next();
    }
    CHECK(iter->status().Ok());
    CHECK_EQ(count, N);

    delete iter;
    delete table;
    std::cout << "  Full scan: " << N << " keys in order ✓\n";
    std::cout << "[PASS] Table Iterator\n";
}

// ════════════════════════════════════════════════════════
// BloomFilter 测试
// ════════════════════════════════════════════════════════

void TestBloomFilterBasic() {
    std::cout << "[TEST] BloomFilter Basic...\n";

    BloomFilter bf(10); // 10 bits/key → 约 1% 误判率

    // 创建过滤器
    std::vector<Slice> keys = {Slice("apple"), Slice("banana"),     Slice("cherry"),
                               Slice("date"),  Slice("elderberry"), Slice("fig"),
                               Slice("grape"), Slice("honeydew")};
    std::string        filter;
    bf.CreateFilter(keys, &filter);

    // 已插入的 key 必须全部返回 true（无假阴性）
    for (const Slice& k : keys) {
        CHECK(bf.KeyMayMatch(k, Slice(filter)));
    }

    // 统计未插入 key 的假阳性率
    int       false_positives = 0;
    const int kTests          = 10000;
    for (int i = 0; i < kTests; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "nonexistent_%04d", i);
        if (bf.KeyMayMatch(Slice(buf), Slice(filter))) {
            false_positives++;
        }
    }
    double fp_rate = 100.0 * false_positives / kTests;
    // 10 bits/key 时理论误判率 ≈ 1%，允许 3% 以内
    CHECK(fp_rate < 3.0);

    std::cout << "  All " << keys.size() << " inserted keys found ✓\n";
    std::cout << "  False positive rate: " << fp_rate << "% (< 3%) ✓\n";
    std::cout << "[PASS] BloomFilter Basic\n";
}

void TestBloomFilterLarge() {
    std::cout << "[TEST] BloomFilter Large (1000 keys)...\n";

    BloomFilter              bf(10);
    std::vector<Slice>       key_slices;
    std::vector<std::string> key_strs; // 保持生命周期

    const int N = 1000;
    key_strs.reserve(N);
    for (int i = 0; i < N; i++) {
        key_strs.push_back("large_key_" + std::to_string(i));
        key_slices.push_back(Slice(key_strs.back()));
    }

    std::string filter;
    bf.CreateFilter(key_slices, &filter);

    // 全部已插入 key 必须可查到
    int misses = 0;
    for (const Slice& k : key_slices) {
        if (!bf.KeyMayMatch(k, Slice(filter)))
            misses++;
    }
    CHECK_EQ(misses, 0); // 不允许假阴性

    std::cout << "  1000 keys, 0 false negatives ✓\n";
    std::cout << "  Filter size: " << filter.size() << " bytes (" << (filter.size() * 8 / N)
              << " bits/key actual) ✓\n";
    std::cout << "[PASS] BloomFilter Large\n";
}

void TestBloomFilterEmpty() {
    std::cout << "[TEST] BloomFilter Empty...\n";

    BloomFilter        bf(10);
    std::vector<Slice> empty_keys;
    std::string        filter;
    bf.CreateFilter(empty_keys, &filter);

    // 空过滤器：任何查询都返回 true（保守策略）
    // 或者返回 false（实现相关）
    // 主要验证不崩溃
    bf.KeyMayMatch(Slice("any_key"), Slice(filter));

    std::cout << "  Empty filter: no crash ✓\n";
    std::cout << "[PASS] BloomFilter Empty\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════

int main() {
    std::cout << "=== minidb Day-07 Tests ===\n\n";

    SetupTestDir();

    // Block 读取
    TestBlockIteratorBasic();
    TestBlockIteratorSeek();

    std::cout << "\n";

    // Table 读写联合
    TestTableOpenAndGet();
    TestTableIterator();

    std::cout << "\n";

    // BloomFilter
    TestBloomFilterBasic();
    TestBloomFilterLarge();
    TestBloomFilterEmpty();

    CleanupTestDir();

    std::cout << "\n All Day-07 tests passed!\n";
    return 0;
}