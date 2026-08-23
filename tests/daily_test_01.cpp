// tests/day01_test.cpp
// 第1天的测试：验证 Slice、Status、编解码工具的正确性

#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "util/coding.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

using namespace mapledb;

// ── 测试辅助宏 ────────────────────────────────────────
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAILED: %s  at %s:%d\n", #expr, __FILE__, __LINE__);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))

// ════════════════════════════════════════════════════════
// Slice 测试
// ════════════════════════════════════════════════════════
void TestSlice() {
    std::cout << "[TEST] Slice...\n";

    // 基本构造和访问
    Slice s1("hello");
    CHECK_EQ(s1.Size(), 5u);
    CHECK(!s1.Empty());
    CHECK_EQ(s1[0], 'h');

    // 从 std::string 构造（注意 string 要在 Slice 生命周期内）
    std::string str = "world";
    Slice       s2(str);
    CHECK_EQ(s2.Size(), 5u);

    // 比较
    CHECK(s1 < s2); // "hello" < "world"
    CHECK(s2 > s1);
    CHECK_NE(s1, s2);

    Slice s3("hello");
    CHECK_EQ(s1, s3);

    // RemovePrefix
    Slice s4("foobar");
    s4.RemovePrefix(3);
    CHECK_EQ(s4, Slice("bar"));

    // StartsWith
    Slice s5("foobar");
    CHECK(s5.StartsWith(Slice("foo")));
    CHECK(!s5.StartsWith(Slice("bar")));

    // ToString
    CHECK_EQ(s1.ToString(), std::string("hello"));

    // 空 Slice
    Slice empty;
    CHECK(empty.Empty());
    CHECK_EQ(empty.Size(), 0u);

    std::cout << "[PASS] Slice\n";
}

// ════════════════════════════════════════════════════════
// Status 测试
// ════════════════════════════════════════════════════════
void TestStatus() {
    std::cout << "[TEST] Status...\n";

    // OK
    Status ok = Status::OK();
    CHECK(ok.Ok());
    CHECK(!ok.IsNotFound());
    CHECK_EQ(ok.ToString(), std::string("OK"));

    // NotFound
    Status nf = Status::NotFound("key not exist", "mykey");
    CHECK(!nf.Ok());
    CHECK(nf.IsNotFound());
    std::string msg = nf.ToString();
    CHECK(msg.find("NotFound") != std::string::npos);
    std::cout << "  NotFound msg: " << msg << "\n";

    // IOError
    Status io = Status::IOError("/data/sst_001.ldb");
    CHECK(io.IsIOError());
    std::cout << "  IOError msg: " << io.ToString() << "\n";

    // 拷贝构造
    Status copy = nf;
    CHECK(copy.IsNotFound());

    // 移动构造
    Status moved = std::move(io);
    CHECK(moved.IsIOError());

    // 赋值
    Status assigned;
    assigned = nf;
    CHECK(assigned.IsNotFound());

    std::cout << "[PASS] Status\n";
}

// ════════════════════════════════════════════════════════
// Fixed 编解码测试
// ════════════════════════════════════════════════════════
void TestFixedCoding() {
    std::cout << "[TEST] Fixed Coding...\n";

    // Fixed32
    {
        std::string buf;
        PutFixed32(&buf, 0x12345678u);
        CHECK_EQ(buf.size(), 4u);
        uint32_t val = DecodeFixed32(buf.data());
        CHECK_EQ(val, 0x12345678u);
    }

    // Fixed64
    {
        std::string buf;
        PutFixed64(&buf, 0xDEADBEEFCAFEBABEull);
        CHECK_EQ(buf.size(), 8u);
        uint64_t val = DecodeFixed64(buf.data());
        CHECK_EQ(val, 0xDEADBEEFCAFEBABEull);
    }

    // 边界值
    {
        std::string buf;
        PutFixed32(&buf, 0u);
        PutFixed32(&buf, UINT32_MAX);
        CHECK_EQ(DecodeFixed32(buf.data()), 0u);
        CHECK_EQ(DecodeFixed32(buf.data() + 4), UINT32_MAX);
    }

    std::cout << "[PASS] Fixed Coding\n";
}

// ════════════════════════════════════════════════════════
// Varint 编解码测试
// ════════════════════════════════════════════════════════
void TestVarintCoding() {
    std::cout << "[TEST] Varint Coding...\n";

    // 测试不同范围的值
    uint32_t test_values[] = {
        0,
        1,
        127, // 1字节
        128,
        300,
        16383, // 2字节
        16384,
        2097151,       // 3字节
        (1 << 28) - 1, // 4字节
        UINT32_MAX     // 5字节
    };

    for (uint32_t v : test_values) {
        std::string buf;
        PutVarint32(&buf, v);

        // 验证编码字节数
        if (v < (1 << 7))
            CHECK_EQ(buf.size(), 1u);
        else if (v < (1 << 14))
            CHECK_EQ(buf.size(), 2u);
        else if (v < (1 << 21))
            CHECK_EQ(buf.size(), 3u);
        else if (v < (1 << 28))
            CHECK_EQ(buf.size(), 4u);
        else
            CHECK_EQ(buf.size(), 5u);

        // 解码验证
        Slice    input(buf);
        uint32_t decoded;
        bool     ok = GetVarint32(&input, &decoded);
        CHECK(ok);
        CHECK_EQ(decoded, v);
        CHECK(input.Empty()); // 所有字节都被消耗
    }

    // Varint64 测试
    {
        std::string buf;
        uint64_t    big = (uint64_t) 1 << 35;
        PutVarint64(&buf, big);
        Slice    input(buf);
        uint64_t decoded;
        CHECK(GetVarint64(&input, &decoded));
        CHECK_EQ(decoded, big);
    }

    std::cout << "[PASS] Varint Coding\n";
}

// ════════════════════════════════════════════════════════
// LengthPrefixed Slice 测试
// ════════════════════════════════════════════════════════
void TestLengthPrefixedSlice() {
    std::cout << "[TEST] LengthPrefixed Slice...\n";

    // 写入两个 key-value（模拟 WAL/SSTable 存储格式）
    std::string buf;
    Slice       key1("name");
    Slice       val1("alice");
    Slice       key2("age");
    Slice       val2("25");

    PutLengthPrefixedSlice(&buf, key1);
    PutLengthPrefixedSlice(&buf, val1);
    PutLengthPrefixedSlice(&buf, key2);
    PutLengthPrefixedSlice(&buf, val2);

    // 读取验证
    Slice input(buf);
    Slice result;

    CHECK(GetLengthPrefixedSlice(&input, &result));
    CHECK_EQ(result, key1);

    CHECK(GetLengthPrefixedSlice(&input, &result));
    CHECK_EQ(result, val1);

    CHECK(GetLengthPrefixedSlice(&input, &result));
    CHECK_EQ(result, key2);

    CHECK(GetLengthPrefixedSlice(&input, &result));
    CHECK_EQ(result, val2);

    CHECK(input.Empty()); // 所有数据都被消耗

    std::cout << "[PASS] LengthPrefixed Slice\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════
int main() {
    std::cout << "=== minidb Day-01 Tests ===\n\n";

    TestSlice();
    TestStatus();
    TestFixedCoding();
    TestVarintCoding();
    TestLengthPrefixedSlice();

    std::cout << "\n All Day-01 tests passed!\n";
    return 0;
}