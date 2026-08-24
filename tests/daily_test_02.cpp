/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-24 16:57:14
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-24 16:57:47
 * @Description:
 */

#include "util/arena.h"
#include "util/random.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

using namespace mapledb;

// ── 测试宏 ────────────────────────────────────────────
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAILED: %s at %s:%d\n", #expr, __FILE__, __LINE__);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// ════════════════════════════════════════════════════════
// Arena 测试
// ════════════════════════════════════════════════════════

void TestArenaBasic() {
    std::cout << "[TEST] Arena Basic...\n";

    Arena arena;

    // 初始状态：未分配任何内存
    CHECK_EQ(arena.MemoryUsage(), 0u);

    // 分配一小块，验证可以正常写入
    const size_t N = 100;
    char*        p = arena.Allocate(N);
    CHECK(p != nullptr);
    memset(p, 0xAB, N); // 填充标志字节
    for (size_t i = 0; i < N; i++) {
        CHECK_EQ((unsigned char) p[i], 0xABu);
    }

    // 内存用量应该增加了（至少一个 Block 的大小）
    CHECK(arena.MemoryUsage() > 0);

    std::cout << "  After 100-byte alloc, MemoryUsage = " << arena.MemoryUsage() << " bytes\n";

    std::cout << "[PASS] Arena Basic\n";
}

void TestArenaMultipleAllocs() {
    std::cout << "[TEST] Arena Multiple Allocs...\n";

    Arena arena;

    // 分配很多小块，验证：
    // 1. 指针不重叠（每次分配的区域互不干扰）
    // 2. 写入的数据不被后续分配覆盖
    const int           kCount = 1000;
    std::vector<char*>  ptrs;
    std::vector<size_t> sizes;

    Random rnd(42);
    for (int i = 0; i < kCount; i++) {
        // 随机分配 1~200 字节
        size_t sz = rnd.Uniform(200) + 1;
        char*  p  = arena.Allocate(sz);
        CHECK(p != nullptr);

        // 用索引 i 填充，方便后续验证
        memset(p, static_cast<char>(i & 0xFF), sz);

        ptrs.push_back(p);
        sizes.push_back(sz);
    }

    // 验证所有分配的内存内容没有被覆盖
    for (int i = 0; i < kCount; i++) {
        for (size_t j = 0; j < sizes[i]; j++) {
            CHECK_EQ((unsigned char) ptrs[i][j], (unsigned char) (i & 0xFF));
        }
    }

    std::cout << "  Allocated " << kCount << " random-size chunks, "
              << "total MemoryUsage = " << arena.MemoryUsage() << " bytes\n";

    std::cout << "[PASS] Arena Multiple Allocs\n";
}

void TestArenaLargeAlloc() {
    std::cout << "[TEST] Arena Large Alloc...\n";

    Arena arena;

    // 分配超过 kBlockSize/4 的大对象，验证走单独 Block 路径
    // kBlockSize = 4096，kBlockSize/4 = 1024
    size_t large_size = 2000; // > 1024，走大对象路径
    char*  p          = arena.Allocate(large_size);
    CHECK(p != nullptr);
    memset(p, 0x55, large_size);

    // 分配大对象后，接着分配小对象，验证小对象的 Block 没有被破坏
    char* small = arena.Allocate(100);
    CHECK(small != nullptr);
    memset(small, 0xAA, 100);

    // 验证大对象数据完整
    for (size_t i = 0; i < large_size; i++) {
        CHECK_EQ((unsigned char) p[i], 0x55u);
    }
    // 验证小对象数据完整
    for (size_t i = 0; i < 100; i++) {
        CHECK_EQ((unsigned char) small[i], 0xAAu);
    }

    std::cout << "  Large alloc (" << large_size << " bytes) and small alloc coexist correctly\n";

    std::cout << "[PASS] Arena Large Alloc\n";
}

void TestArenaAligned() {
    std::cout << "[TEST] Arena Aligned Alloc...\n";

    Arena arena;

    // 验证 AllocateAligned 返回的指针满足对齐要求
    // 在 64 位系统上，对齐值为 8
    const size_t align = (sizeof(void*) > 8) ? sizeof(void*) : 8;

    for (int i = 0; i < 50; i++) {
        // 故意用奇数大小，触发对齐填充逻辑
        size_t sz = i * 3 + 1;
        char*  p  = arena.AllocateAligned(sz);
        CHECK(p != nullptr);

        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        CHECK_EQ(addr % align, 0u); // 必须是 align 的整数倍
    }

    std::cout << "  All aligned allocs are " << align << "-byte aligned\n";
    std::cout << "[PASS] Arena Aligned Alloc\n";
}

// Arena 性能对比：Arena vs new/delete
void TestArenaPerformance() {
    std::cout << "[TEST] Arena vs new/delete Performance...\n";

    const int kCount = 100000;
    const int kSize  = 64; // 模拟 SkipList 节点大小

    // ── 方法1：Arena 分配 ─────────────────────────────────
    auto t1 = std::chrono::steady_clock::now();
    {
        Arena arena;
        for (int i = 0; i < kCount; i++) {
            char* p = arena.Allocate(kSize);
            (void) p; // 防止被优化掉
        }
        // arena 析构时统一释放，不计入分配时间
    }
    auto t2       = std::chrono::steady_clock::now();
    auto arena_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

    // ── 方法2：new/delete 分配 ────────────────────────────
    auto t3 = std::chrono::steady_clock::now();
    {
        std::vector<char*> ptrs;
        ptrs.reserve(kCount);
        for (int i = 0; i < kCount; i++) {
            ptrs.push_back(new char[kSize]);
        }
        for (char* p : ptrs) {
            delete[] p;
        }
    }
    auto t4        = std::chrono::steady_clock::now();
    auto newdel_us = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count();

    std::cout << "  " << kCount << " allocs of " << kSize << " bytes:\n";
    std::cout << "    Arena:      " << arena_us << " μs\n";
    std::cout << "    new/delete: " << newdel_us << " μs\n";
    if (arena_us > 0) {
        std::cout << "    Arena speedup: ~" << newdel_us / arena_us << "x\n";
    }

    std::cout << "[PASS] Arena Performance\n";
}

// ════════════════════════════════════════════════════════
// Random 测试
// ════════════════════════════════════════════════════════

void TestRandomBasic() {
    std::cout << "[TEST] Random Basic...\n";

    Random rnd(123);

    // 验证：生成的值在合法范围内
    // LCG 的值域是 [1, 2^31-2]
    const uint32_t M = 2147483647u;
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rnd.Next();
        CHECK(v >= 1u);
        CHECK(v < M);
    }

    std::cout << "  10000 values all in [1, 2^31-2] ✓\n";
    std::cout << "[PASS] Random Basic\n";
}

void TestRandomDeterministic() {
    std::cout << "[TEST] Random Deterministic (same seed = same sequence)...\n";

    // 相同种子必须产生相同序列（可复现性）
    Random rnd1(42);
    Random rnd2(42);

    for (int i = 0; i < 1000; i++) {
        CHECK_EQ(rnd1.Next(), rnd2.Next());
    }

    // 不同种子产生不同序列
    Random rnd3(43);
    Random rnd4(44);
    bool   different = false;
    for (int i = 0; i < 100; i++) {
        if (rnd3.Next() != rnd4.Next()) {
            different = true;
            break;
        }
    }
    CHECK(different);

    std::cout << "  Same seed → same sequence ✓\n";
    std::cout << "  Different seeds → different sequences ✓\n";
    std::cout << "[PASS] Random Deterministic\n";
}

void TestRandomUniform() {
    std::cout << "[TEST] Random Uniform Distribution...\n";

    Random rnd(12345);

    // 验证 Uniform(n) 的分布均匀性（简单验证：各桶出现次数差距不太大）
    const int kN         = 4;
    const int kSamples   = 100000;
    int       counts[kN] = {};

    for (int i = 0; i < kSamples; i++) {
        int v = static_cast<int>(rnd.Uniform(kN));
        CHECK(v >= 0 && v < kN);
        counts[v]++;
    }

    std::cout << "  Uniform(4) distribution over " << kSamples << " samples:\n";
    for (int i = 0; i < kN; i++) {
        double expected  = kSamples / kN;
        double deviation = std::abs(counts[i] - expected) / expected * 100.0;
        std::cout << "    bucket[" << i << "]: " << counts[i]
                  << "  (deviation from expected: " << deviation << "%)\n";
        // 允许 5% 以内的误差
        CHECK(deviation < 5.0);
    }

    std::cout << "[PASS] Random Uniform\n";
}

void TestRandomOneIn() {
    std::cout << "[TEST] Random OneIn...\n";

    Random rnd(999);

    // OneIn(4) 的真实概率应该接近 25%
    const int kSamples   = 100000;
    int       true_count = 0;
    for (int i = 0; i < kSamples; i++) {
        if (rnd.OneIn(4))
            true_count++;
    }

    double actual_prob = 100.0 * true_count / kSamples;
    std::cout << "  OneIn(4): expected ~25%, got " << actual_prob << "%\n";

    // 允许 ±2% 误差
    CHECK(actual_prob > 23.0 && actual_prob < 27.0);

    std::cout << "[PASS] Random OneIn\n";
}

// 演示 SkipList 用到的 RandomHeight 逻辑（预告 Day3）
void TestRandomSkipListHeight() {
    std::cout << "[TEST] Random SkipList Height Simulation...\n";

    // SkipList 使用 OneIn(4) 生成随机高度，最大 12 层
    // 高度分布应该大约是：
    //   height=1: ~75%
    //   height=2: ~18.75%
    //   height=3: ~4.69%
    //   ...
    const int kMaxHeight                   = 12;
    const int kSamples                     = 100000;
    int       height_count[kMaxHeight + 1] = {};

    Random rnd(42);
    for (int i = 0; i < kSamples; i++) {
        int height = 1;
        while (height < kMaxHeight && rnd.OneIn(4)) {
            height++;
        }
        height_count[height]++;
    }

    std::cout << "  SkipList height distribution (" << kSamples << " samples):\n";
    for (int h = 1; h <= kMaxHeight; h++) {
        if (height_count[h] == 0)
            break;
        double pct = 100.0 * height_count[h] / kSamples;
        // 简易柱状图
        int bars = static_cast<int>(pct / 2);
        std::cout << "    h=" << h << ": ";
        for (int b = 0; b < bars; b++)
            std::cout << "█";
        std::cout << " " << pct << "%\n";
    }

    // 验证 height=1 的比例接近 75%
    double h1_pct = 100.0 * height_count[1] / kSamples;
    CHECK(h1_pct > 70.0 && h1_pct < 80.0);

    std::cout << "[PASS] Random SkipList Height Simulation\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════
int main() {
    std::cout << "=== minidb Day-02 Tests ===\n\n";

    // Arena 测试
    TestArenaBasic();
    TestArenaMultipleAllocs();
    TestArenaLargeAlloc();
    TestArenaAligned();
    TestArenaPerformance();

    std::cout << "\n";

    // Random 测试
    TestRandomBasic();
    TestRandomDeterministic();
    TestRandomUniform();
    TestRandomOneIn();
    TestRandomSkipListHeight();

    std::cout << "\n All Day-02 tests passed!\n";
    return 0;
}