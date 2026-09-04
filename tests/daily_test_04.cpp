// tests/day04_test.cpp
// 第4天测试：dbformat（InternalKey）和 MemTable 的正确性验证

#include "db/dbformat.h"
#include "db/memtable.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace mapledb;

// ── 测试宏 ────────────────────────────────────────────
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAILED: %s  at %s:%d\n", #expr, __FILE__, __LINE__);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

// ════════════════════════════════════════════════════════
// dbformat 测试
// ════════════════════════════════════════════════════════

void TestInternalKeyEncoding() {
    std::cout << "[TEST] InternalKey Encoding...\n";

    // 手动编码一个 InternalKey，验证各字段能正确解码
    std::string encoded;
    AppendInternalKey(&encoded, Slice("hello"), 100, kTypeValue);

    // InternalKey 长度 = user_key 长度 + 8 字节 tag
    CHECK_EQ(encoded.size(), 5u + 8u); // "hello"(5) + tag(8)

    // 解码 user_key
    Slice ikey(encoded);
    Slice ukey = ExtractUserKey(ikey);
    CHECK_EQ(ukey, Slice("hello"));

    // 解码 SequenceNumber 和 ValueType
    SequenceNumber seq = GetSequenceNumber(ikey);
    ValueType      vt  = GetValueType(ikey);
    CHECK_EQ(seq, 100u);
    CHECK_EQ(vt, kTypeValue);

    // 测试 Deletion 类型
    std::string del_encoded;
    AppendInternalKey(&del_encoded, Slice("world"), 200, kTypeDeletion);
    Slice del_ikey(del_encoded);
    CHECK_EQ(GetSequenceNumber(del_ikey), 200u);
    CHECK_EQ(GetValueType(del_ikey), kTypeDeletion);

    std::cout << "  Encoded \"hello\"@100:Value → size=" << encoded.size()
              << "  decoded correctly ✓\n";
    std::cout << "[PASS] InternalKey Encoding\n";
}

void TestInternalKeyComparator() {
    std::cout << "[TEST] InternalKeyComparator...\n";

    InternalKeyComparator cmp;

    // 构造几个 InternalKey
    auto make_ikey = [](const std::string& uk, SequenceNumber seq, ValueType t) {
        std::string s;
        AppendInternalKey(&s, Slice(uk), seq, t);
        return s;
    };

    std::string a = make_ikey("apple", 100, kTypeValue);
    std::string b = make_ikey("banana", 50, kTypeValue);
    std::string c = make_ikey("apple", 200, kTypeValue); // 同 key，更新的版本
    std::string d = make_ikey("apple", 100, kTypeDeletion);

    // "apple" < "banana"（字典序）
    CHECK(cmp.Compare(Slice(a), Slice(b)) < 0);
    CHECK(cmp.Compare(Slice(b), Slice(a)) > 0);

    // 相同 user_key，seq 大的排前面（降序）
    // c(seq=200) 排在 a(seq=100) 前面
    CHECK(cmp.Compare(Slice(c), Slice(a)) < 0); // c < a 在比较器眼中（c 排前面）
    CHECK(cmp.Compare(Slice(a), Slice(c)) > 0);

    // 相同 user_key，相同 seq，Value > Deletion（tag 值更大）
    // d(Deletion) 的 tag = (100<<8|0)，a(Value) 的 tag = (100<<8|1)
    // tag_a > tag_d → a 排前面 → cmp(a,d) < 0
    CHECK(cmp.Compare(Slice(a), Slice(d)) < 0);

    // 自身比较
    CHECK_EQ(cmp.Compare(Slice(a), Slice(a)), 0);

    std::cout << "  apple@200 > apple@100 (seq descending) ✓\n";
    std::cout << "  apple < banana (user_key ascending) ✓\n";
    std::cout << "[PASS] InternalKeyComparator\n";
}

void TestLookupKey() {
    std::cout << "[TEST] LookupKey...\n";

    LookupKey lk("hello", 100);

    // user_key 正确
    CHECK_EQ(lk.user_key(), Slice("hello"));

    // internal_key = "hello" + tag(100, kValueTypeForSeek)
    Slice ikey = lk.internal_key();
    CHECK_EQ(ikey.Size(), 5u + 8u);
    CHECK_EQ(ExtractUserKey(ikey), Slice("hello"));
    CHECK_EQ(GetSequenceNumber(ikey), 100u);
    CHECK_EQ(GetValueType(ikey), kValueTypeForSeek);

    // memtable_key = varint32(13) + internal_key
    Slice mkey = lk.memtable_key();
    CHECK(mkey.Size() > ikey.Size()); // 多了 varint 前缀

    // 测试长 key（超过 space_ 的 200 字节，触发堆分配）
    std::string long_key(210, 'x');
    LookupKey   lk_long(Slice(long_key), 999);
    CHECK_EQ(lk_long.user_key(), Slice(long_key));
    CHECK_EQ(GetSequenceNumber(lk_long.internal_key()), 999u);

    std::cout << "  memtable_key size=" << mkey.Size() << ", internal_key size=" << ikey.Size()
              << " ✓\n";
    std::cout << "  Long key (heap alloc) works ✓\n";
    std::cout << "[PASS] LookupKey\n";
}

// ════════════════════════════════════════════════════════
// MemTable 测试
// ════════════════════════════════════════════════════════

// 辅助函数：创建 MemTable（引用计数管理）
MemTable* NewMemTable() {
    InternalKeyComparator cmp;
    MemTable*             mem = new MemTable(cmp);
    mem->Ref();
    return mem;
}

void TestMemTableBasicPutGet() {
    std::cout << "[TEST] MemTable Basic Put/Get...\n";

    MemTable* mem = NewMemTable();

    // Put 几条记录（seq 单调递增）
    mem->Add(1, kTypeValue, "name", "alice");
    mem->Add(2, kTypeValue, "city", "beijing");
    mem->Add(3, kTypeValue, "score", "100");

    // Get 验证
    std::string value;
    Status      s;

    // 查最新版本（用 kMaxSequenceNumber）
    LookupKey lk1("name", kMaxSequenceNumber);
    bool      found = mem->Get(lk1, &value, &s);
    CHECK(found);
    CHECK(s.Ok());
    CHECK_EQ(value, std::string("alice"));

    LookupKey lk2("city", kMaxSequenceNumber);
    found = mem->Get(lk2, &value, &s);
    CHECK(found);
    CHECK_EQ(value, std::string("beijing"));

    // 查不存在的 key
    LookupKey lk3("age", kMaxSequenceNumber);
    found = mem->Get(lk3, &value, &s);
    CHECK(!found); // false 表示 MemTable 里没有这个 key

    std::cout << "  Put 3 entries, all Get correctly ✓\n";
    std::cout << "  Non-existent key returns false ✓\n";

    mem->Unref();
    std::cout << "[PASS] MemTable Basic Put/Get\n";
}

void TestMemTableMultiVersion() {
    std::cout << "[TEST] MemTable Multi-Version (MVCC)...\n";

    MemTable* mem = NewMemTable();

    // 对同一个 key 多次写入（模拟更新操作）
    mem->Add(1, kTypeValue, "key", "value_v1");
    mem->Add(2, kTypeValue, "key", "value_v2");
    mem->Add(3, kTypeValue, "key", "value_v3");

    std::string value;
    Status      s;

    // 用最大序列号查询 → 应该得到最新版本 v3
    LookupKey lk_latest("key", kMaxSequenceNumber);
    CHECK(mem->Get(lk_latest, &value, &s));
    CHECK(s.Ok());
    CHECK_EQ(value, std::string("value_v3"));

    // 用 seq=2 查询 → 应该得到 v2（能看到 seq<=2 的最新写入）
    LookupKey lk_v2("key", 2);
    CHECK(mem->Get(lk_v2, &value, &s));
    CHECK(s.Ok());
    CHECK_EQ(value, std::string("value_v2"));

    // 用 seq=1 查询 → 应该得到 v1
    LookupKey lk_v1("key", 1);
    CHECK(mem->Get(lk_v1, &value, &s));
    CHECK(s.Ok());
    CHECK_EQ(value, std::string("value_v1"));

    std::cout << "  seq=MAX → v3, seq=2 → v2, seq=1 → v1  ✓\n";

    mem->Unref();
    std::cout << "[PASS] MemTable Multi-Version\n";
}

void TestMemTableDelete() {
    std::cout << "[TEST] MemTable Delete (tombstone)...\n";

    MemTable* mem = NewMemTable();

    // 写入一条记录
    mem->Add(1, kTypeValue, "key", "hello");

    std::string value;
    Status      s;

    // 先验证能查到
    LookupKey lk1("key", kMaxSequenceNumber);
    CHECK(mem->Get(lk1, &value, &s));
    CHECK(s.Ok());
    CHECK_EQ(value, std::string("hello"));

    // 写入删除标记（seq=2 > seq=1，覆盖旧版本）
    mem->Add(2, kTypeDeletion, "key", "");

    // 再查询 → 应该返回 NotFound
    LookupKey lk2("key", kMaxSequenceNumber);
    bool      found = mem->Get(lk2, &value, &s);
    CHECK(found);          // true：在 MemTable 找到了答案（答案是：已删除）
    CHECK(s.IsNotFound()); // Status 是 NotFound

    // 用 seq=1 查询（"时间旅行"到删除之前）→ 还能看到旧值
    LookupKey lk3("key", 1);
    CHECK(mem->Get(lk3, &value, &s));
    CHECK(s.Ok());
    CHECK_EQ(value, std::string("hello"));

    std::cout << "  After Delete(seq=2): Get returns NotFound ✓\n";
    std::cout << "  Get(seq=1): still sees old value ✓\n";

    mem->Unref();
    std::cout << "[PASS] MemTable Delete\n";
}

void TestMemTableIterator() {
    std::cout << "[TEST] MemTable Iterator (ordered)...\n";

    MemTable* mem = NewMemTable();

    // 乱序插入，验证迭代器按 InternalKey 顺序输出
    mem->Add(3, kTypeValue, "cherry", "c");
    mem->Add(1, kTypeValue, "apple", "a");
    mem->Add(2, kTypeValue, "banana", "b");
    mem->Add(4, kTypeValue, "apple", "a2"); // apple 的第二个版本

    // 用迭代器顺序读出所有条目
    MemTable::Table::Iterator* iter = mem->NewIterator();
    iter->SeekToFirst();

    // InternalKeyComparator 排序：
    //   apple@4:Value  (seq=4，最新，排最前)
    //   apple@1:Value  (seq=1，旧版本)
    //   banana@2:Value
    //   cherry@3:Value
    std::vector<std::pair<std::string, std::string>> expected = {
        {"apple", "a2"}, // seq=4
        {"apple", "a"},  // seq=1
        {"banana", "b"},
        {"cherry", "c"},
    };

    for (auto& [exp_key, exp_val] : expected) {
        CHECK(iter->Valid());

        const char* entry = iter->key();
        // 解码 user_key
        uint32_t    ik_len;
        const char* ik_start = GetVarint32Ptr(entry, entry + 5, &ik_len);
        Slice       user_key(ik_start, ik_len - 8);
        CHECK_EQ(user_key.ToString(), exp_key);

        // 解码 value
        const char* val_ptr = ik_start + ik_len;
        uint32_t    val_len;
        const char* val_start = GetVarint32Ptr(val_ptr, val_ptr + 5, &val_len);
        std::string val(val_start, val_len);
        CHECK_EQ(val, exp_val);

        iter->Next();
    }
    CHECK(!iter->Valid());

    delete iter;
    mem->Unref();

    std::cout << "  Order: apple@4, apple@1, banana@2, cherry@3 ✓\n";
    std::cout << "[PASS] MemTable Iterator\n";
}

void TestMemTableMemoryUsage() {
    std::cout << "[TEST] MemTable Memory Usage...\n";

    InternalKeyComparator cmp;
    MemTable*             mem = new MemTable(cmp);
    mem->Ref();

    size_t before = mem->ApproximateMemoryUsage();

    const int N = 1000;
    for (int i = 0; i < N; i++) {
        std::string key   = "key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);
        mem->Add(static_cast<SequenceNumber>(i), kTypeValue, Slice(key), Slice(value));
    }

    size_t after = mem->ApproximateMemoryUsage();
    CHECK(after > before);
    CHECK(after < (size_t) N * 1024); // 不应超过 1KB/entry（合理上界）

    std::cout << "  " << N << " entries, memory usage = " << after / 1024 << " KB  (" << after / N
              << " bytes/entry avg)\n";

    mem->Unref();
    std::cout << "[PASS] MemTable Memory Usage\n";
}

void TestMemTableRefCounting() {
    std::cout << "[TEST] MemTable Ref Counting...\n";

    // 测试引用计数机制：多次 Ref/Unref，确保最后一次 Unref 时正确销毁
    InternalKeyComparator cmp;
    MemTable*             mem = new MemTable(cmp);

    mem->Ref(); // refs = 1
    mem->Ref(); // refs = 2
    mem->Ref(); // refs = 3

    mem->Unref(); // refs = 2，不销毁
    mem->Unref(); // refs = 1，不销毁

    // 此时 mem 还有效
    mem->Add(1, kTypeValue, "test", "value");

    std::string val;
    Status      s;
    LookupKey   lk("test", kMaxSequenceNumber);
    CHECK(mem->Get(lk, &val, &s));
    CHECK_EQ(val, std::string("value"));

    mem->Unref(); // refs = 0 → 自动 delete

    // mem 此后不可再访问（已销毁）

    std::cout << "  Ref/Unref lifecycle correct ✓\n";
    std::cout << "[PASS] MemTable Ref Counting\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════
int main() {
    std::cout << "=== minidb Day-04 Tests ===\n\n";

    // dbformat 测试
    TestInternalKeyEncoding();
    TestInternalKeyComparator();
    TestLookupKey();

    std::cout << "\n";

    // MemTable 测试
    TestMemTableBasicPutGet();
    TestMemTableMultiVersion();
    TestMemTableDelete();
    TestMemTableIterator();
    TestMemTableMemoryUsage();
    TestMemTableRefCounting();

    std::cout << "\n All Day-04 tests passed!\n";
    return 0;
}