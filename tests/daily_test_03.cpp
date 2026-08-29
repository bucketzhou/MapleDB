/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-29 09:51:28
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-29 09:55:54
 * @Description: 第3天测试：验证 SkipList 的正确性（功能、有序性、迭代器、并发）
 */

#include "db/skiplist.h"
#include "util/arena.h"
#include "util/random.h"

#include <atomic>
#include <cassert>
#include <climits>
#include <cstdio>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <thread>
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
// 比较器：用于 int 类型 key 的 SkipList
// ════════════════════════════════════════════════════════
struct IntComparator {
    int operator()(int a, int b) const {
        if (a < b)
            return -1;
        if (a > b)
            return +1;
        return 0;
    }
};

// 方便起见，定义两个类型别名
using IntSkipList = SkipList<int, IntComparator>;

// ════════════════════════════════════════════════════════
// 测试：空跳表
// ════════════════════════════════════════════════════════
void TestEmpty() {
    std::cout << "[TEST] Empty SkipList...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    CHECK(!list.Contains(1));
    CHECK(!list.Contains(100));
    CHECK(!list.Contains(-1));

    // 空跳表的迭代器
    IntSkipList::Iterator iter(&list);
    CHECK(!iter.Valid());

    iter.SeekToFirst();
    CHECK(!iter.Valid());

    iter.SeekToLast();
    CHECK(!iter.Valid());

    iter.Seek(42);
    CHECK(!iter.Valid());

    std::cout << "[PASS] Empty SkipList\n";
}

// ════════════════════════════════════════════════════════
// 测试：基本插入与查找
// ════════════════════════════════════════════════════════
void TestInsertAndContains() {
    std::cout << "[TEST] Insert and Contains...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    // 插入一些 key
    std::vector<int> keys = {3, 1, 7, 5, 2, 9, 4, 8, 6, 10};
    for (int k : keys) {
        list.Insert(k);
    }

    // 验证全部存在
    for (int k : keys) {
        CHECK(list.Contains(k));
    }

    // 验证不存在的 key
    CHECK(!list.Contains(0));
    CHECK(!list.Contains(11));
    CHECK(!list.Contains(100));

    std::cout << "  Inserted " << keys.size() << " keys, all found ✓\n";
    std::cout << "[PASS] Insert and Contains\n";
}

// ════════════════════════════════════════════════════════
// 测试：迭代器正向遍历（有序性验证）
// ════════════════════════════════════════════════════════
void TestIteratorForward() {
    std::cout << "[TEST] Iterator Forward (order)...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    // 乱序插入 0~99
    Random        rnd(42);
    std::set<int> inserted;
    for (int i = 0; i < 100; i++) {
        int key = static_cast<int>(rnd.Uniform(200));
        if (inserted.find(key) == inserted.end()) {
            list.Insert(key);
            inserted.insert(key);
        }
    }

    // 用迭代器遍历，验证顺序严格递增
    IntSkipList::Iterator iter(&list);
    iter.SeekToFirst();

    int prev_key = INT_MIN;
    int count    = 0;
    while (iter.Valid()) {
        int cur = iter.key();
        CHECK(cur > prev_key);           // 严格递增（无重复，跳表不允许重复 key）
        CHECK(inserted.count(cur) == 1); // 只有插入过的 key
        prev_key = cur;
        count++;
        iter.Next();
    }

    CHECK_EQ(count, (int) inserted.size());
    std::cout << "  Traversed " << count << " keys in ascending order ✓\n";
    std::cout << "[PASS] Iterator Forward\n";
}

// ════════════════════════════════════════════════════════
// 测试：迭代器 Seek 定位
// ════════════════════════════════════════════════════════
void TestIteratorSeek() {
    std::cout << "[TEST] Iterator Seek...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    // 插入偶数 0, 2, 4, ..., 18
    for (int i = 0; i <= 18; i += 2) {
        list.Insert(i);
    }

    IntSkipList::Iterator iter(&list);

    // Seek 到已存在的 key
    iter.Seek(4);
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 4);

    // Seek 到不存在的奇数 key（应该定位到下一个偶数）
    iter.Seek(5);
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 6); // >= 5 的第一个 key 是 6

    iter.Seek(7);
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 8);

    // Seek 到比所有 key 都小的值（应该定位到第一个）
    iter.Seek(-1);
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 0);

    // Seek 到比所有 key 都大的值（应该无效）
    iter.Seek(100);
    CHECK(!iter.Valid());

    // SeekToFirst / SeekToLast
    iter.SeekToFirst();
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 0);

    iter.SeekToLast();
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 18);

    std::cout << "  Seek tests all passed ✓\n";
    std::cout << "[PASS] Iterator Seek\n";
}

// ════════════════════════════════════════════════════════
// 测试：迭代器 Prev（反向）
// ════════════════════════════════════════════════════════
void TestIteratorPrev() {
    std::cout << "[TEST] Iterator Prev...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    for (int i = 1; i <= 5; i++) {
        list.Insert(i);
    }

    IntSkipList::Iterator iter(&list);

    // 从尾到头反向遍历
    iter.SeekToLast();
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 5);

    iter.Prev();
    CHECK(iter.Valid());
    CHECK_EQ(iter.key(), 4);

    iter.Prev();
    CHECK_EQ(iter.key(), 3);

    iter.Prev();
    CHECK_EQ(iter.key(), 2);

    iter.Prev();
    CHECK_EQ(iter.key(), 1);

    iter.Prev();
    CHECK(!iter.Valid()); // 已经到头了

    std::cout << "  Reverse traversal: 5->4->3->2->1->invalid ✓\n";
    std::cout << "[PASS] Iterator Prev\n";
}

// ════════════════════════════════════════════════════════
// 测试：大规模插入与正确性（和 std::set 对比）
// ════════════════════════════════════════════════════════
void TestLargeScale() {
    std::cout << "[TEST] Large Scale (vs std::set)...\n";

    Arena         arena;
    IntSkipList   list(IntComparator{}, &arena);
    std::set<int> reference;

    Random    rnd(12345);
    const int N = 10000;

    // 随机插入 N 个不重复的 key
    for (int i = 0; i < N;) {
        int key = static_cast<int>(rnd.Uniform(N * 2));
        if (reference.find(key) == reference.end()) {
            list.Insert(key);
            reference.insert(key);
            i++;
        }
    }

    // 验证 Contains 与 set 一致
    for (int key = 0; key < N * 2; key++) {
        bool in_list = list.Contains(key);
        bool in_set  = (reference.count(key) > 0);
        CHECK_EQ(in_list, in_set);
    }

    // 验证迭代器遍历结果与 set 完全一致（有序）
    IntSkipList::Iterator iter(&list);
    iter.SeekToFirst();
    auto set_it = reference.begin();
    int  count  = 0;
    while (iter.Valid()) {
        CHECK(set_it != reference.end());
        CHECK_EQ(iter.key(), *set_it);
        iter.Next();
        ++set_it;
        count++;
    }
    CHECK(set_it == reference.end());
    CHECK_EQ(count, N);

    std::cout << "  " << N << " elements: Contains and Iterator match std::set ✓\n";
    std::cout << "[PASS] Large Scale\n";
}

// ════════════════════════════════════════════════════════
// 测试：并发读写安全性
//
// 场景：1 个写线程持续插入，多个读线程同时 Seek 遍历
// 验证：读线程不崩溃，遍历结果有序（不跳过已插入的元素）
// ════════════════════════════════════════════════════════
void TestConcurrentReadWrite() {
    std::cout << "[TEST] Concurrent Read + Write...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    // 先插入 0~999（初始状态）
    for (int i = 0; i < 1000; i++) {
        list.Insert(i);
    }

    std::atomic<bool> stop{false};
    std::atomic<int>  write_count{0};

    // 写线程：继续插入 1000~4999
    std::thread writer([&]() {
        for (int i = 1000; i < 5000; i++) {
            list.Insert(i);
            write_count.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_release);
    });

    // 读线程：不断 Seek 遍历，验证有序性
    auto reader_fn = [&](int seed) {
        Random rnd(seed);
        int    iterations = 0;
        while (!stop.load(std::memory_order_acquire) || iterations < 10) {
            // 随机选一个起点 Seek
            int                   target = static_cast<int>(rnd.Uniform(2000));
            IntSkipList::Iterator iter(&list);
            iter.Seek(target);

            // 向前走最多 100 步，验证顺序单调递增
            int prev  = INT_MIN;
            int steps = 0;
            while (iter.Valid() && steps < 100) {
                CHECK(iter.key() >= prev);   // 必须单调不减（允许 == 因为只检查，不插入重复）
                CHECK(iter.key() >= target); // Seek 结果必须 >= target
                prev = iter.key();
                iter.Next();
                steps++;
            }
            iterations++;
        }
    };

    // 启动 3 个读线程
    std::thread r1(reader_fn, 111);
    std::thread r2(reader_fn, 222);
    std::thread r3(reader_fn, 333);

    writer.join();
    r1.join();
    r2.join();
    r3.join();

    std::cout << "  Writer inserted 4000 keys, 3 readers ran concurrently ✓\n";
    std::cout << "  No crash, all reads were monotonically ordered ✓\n";
    std::cout << "[PASS] Concurrent Read + Write\n";
}

// ════════════════════════════════════════════════════════
// 测试：Arena 内存使用量合理
// ════════════════════════════════════════════════════════
void TestArenaUsage() {
    std::cout << "[TEST] Arena Memory Usage...\n";

    Arena       arena;
    IntSkipList list(IntComparator{}, &arena);

    const int N = 5000;
    for (int i = 0; i < N; i++) {
        list.Insert(i);
    }

    size_t usage = arena.MemoryUsage();
    // 每个节点至少 sizeof(int) + 1个指针，粗略估算上界
    // 实际应该远小于 N * 1024（说明没有严重浪费）
    CHECK(usage > 0);
    CHECK(usage < (size_t) N * 1024);

    std::cout << "  " << N << " nodes, Arena usage = " << usage / 1024 << " KB\n";
    std::cout << "  Per-node avg = " << usage / N << " bytes\n";
    std::cout << "[PASS] Arena Memory Usage\n";
}

// ════════════════════════════════════════════════════════
// 测试：用 string key 演示泛型（为 MemTable 铺路）
// ════════════════════════════════════════════════════════
void TestStringKey() {
    std::cout << "[TEST] String Key SkipList...\n";

    // 字符串比较器
    struct StrCmp {
        int operator()(const std::string& a, const std::string& b) const { return a.compare(b); }
    };

    Arena                         arena;
    SkipList<std::string, StrCmp> list(StrCmp{}, &arena);

    list.Insert("banana");
    list.Insert("apple");
    list.Insert("cherry");
    list.Insert("date");

    CHECK(list.Contains("apple"));
    CHECK(list.Contains("banana"));
    CHECK(!list.Contains("elderberry"));

    // 按字母序遍历
    SkipList<std::string, StrCmp>::Iterator iter(&list);
    iter.SeekToFirst();

    std::vector<std::string> expected = {"apple", "banana", "cherry", "date"};
    for (const auto& exp : expected) {
        CHECK(iter.Valid());
        CHECK_EQ(iter.key(), exp);
        iter.Next();
    }
    CHECK(!iter.Valid());

    std::cout << "  apple -> banana -> cherry -> date ✓\n";
    std::cout << "[PASS] String Key SkipList\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════
int main() {
    std::cout << "=== minidb Day-03 Tests ===\n\n";

    TestEmpty();
    TestInsertAndContains();
    TestIteratorForward();
    TestIteratorSeek();
    TestIteratorPrev();
    TestLargeScale();
    TestConcurrentReadWrite();
    TestArenaUsage();
    TestStringKey();

    std::cout << "\n All Day-03 tests passed!\n";
    return 0;
}