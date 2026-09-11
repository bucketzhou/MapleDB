// tests/day06_test.cpp
// 第6天测试：BlockBuilder + TableBuilder（SSTable 写入）

#include "mapledb/options.h"
#include "table/blockbuilder.h"
#include "table/format.h"
#include "table/tablebuilder.h"
#include "util/coding.h"

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

static const std::string kTestDir = "/tmp/minidb_day06_test";
static void              SetupTestDir() { fs::create_directories(kTestDir); }
static void              CleanupTestDir() { fs::remove_all(kTestDir); }
static std::string       TestFile(const std::string& n) { return kTestDir + "/" + n; }

// ════════════════════════════════════════════════════════
// BlockBuilder 测试
// ════════════════════════════════════════════════════════

void TestBlockBuilderBasic() {
    std::cout << "[TEST] BlockBuilder Basic...\n";

    BlockBuilder builder(4); // restart interval = 4（方便测试）

    // 有序插入 kv
    builder.Add("apple", "1");
    builder.Add("banana", "2");
    builder.Add("cherry", "3");
    builder.Add("date", "4");
    // 第 4 个是 restart point（counter 达到 interval）
    builder.Add("elderberry", "5");

    Slice block = builder.Finish();
    CHECK(!block.Empty());

    // Block 末尾是 restarts 数组 + count（fixed32）
    // restarts 数组至少 2 个元素（offset=0 和 "elderberry" 的 offset）
    // 因此 block.size() >= 2*4 + 4 = 12 字节（restarts + count）
    CHECK(block.Size() >= 12u);

    // 验证 restarts count（block 末尾 4 字节）
    const char* p            = block.Data() + block.Size() - 4;
    uint32_t    num_restarts = DecodeFixed32(p);
    CHECK(num_restarts >= 2u); // 至少 2 个 restart point

    std::cout << "  Block size=" << block.Size() << ", num_restarts=" << num_restarts << " ✓\n";
    std::cout << "[PASS] BlockBuilder Basic\n";
}

void TestBlockBuilderPrefixCompression() {
    std::cout << "[TEST] BlockBuilder Prefix Compression (size savings)...\n";

    // 构造有大量共同前缀的 key：user:000001 ... user:001000
    BlockBuilder compressed(16);  // 正常前缀压缩
    BlockBuilder uncompressed(1); // interval=1 使每条都是 restart（无压缩）

    const int N = 200;
    for (int i = 0; i < N; i++) {
        char key[32];
        snprintf(key, sizeof(key), "user:%06d", i);
        char val[16];
        snprintf(val, sizeof(val), "val_%04d", i);
        compressed.Add(Slice(key), Slice(val));
        uncompressed.Add(Slice(key), Slice(val));
    }

    Slice c_block = compressed.Finish();
    Slice u_block = uncompressed.Finish();

    // 前缀压缩的 block 必须比无压缩小
    CHECK(c_block.Size() < u_block.Size());

    double ratio = 100.0 * (1.0 - (double) c_block.Size() / u_block.Size());
    std::cout << "  Compressed=" << c_block.Size() << "B, Uncompressed=" << u_block.Size()
              << "B, savings=" << ratio << "% ✓\n";
    std::cout << "[PASS] BlockBuilder Prefix Compression\n";
}

void TestBlockBuilderReset() {
    std::cout << "[TEST] BlockBuilder Reset...\n";

    BlockBuilder builder(4);
    builder.Add("aaa", "111");
    builder.Add("bbb", "222");
    Slice  b1    = builder.Finish();
    size_t size1 = b1.Size();

    builder.Reset();
    CHECK(builder.empty());

    builder.Add("aaa", "111");
    builder.Add("bbb", "222");
    Slice b2 = builder.Finish();

    // 两次相同内容，大小应该相同
    CHECK_EQ(b2.Size(), size1);

    std::cout << "  Reset and rebuild: same size ✓\n";
    std::cout << "[PASS] BlockBuilder Reset\n";
}

// ════════════════════════════════════════════════════════
// BlockHandle / Footer 测试
// ════════════════════════════════════════════════════════

void TestBlockHandle() {
    std::cout << "[TEST] BlockHandle Encode/Decode...\n";

    BlockHandle h(12345, 4096);
    std::string encoded;
    h.EncodeTo(&encoded);
    CHECK(!encoded.empty());
    CHECK(encoded.size() <= BlockHandle::kMaxEncodedLength);

    BlockHandle h2;
    Slice       s(encoded);
    CHECK(h2.DecodeFrom(&s).Ok());
    CHECK_EQ(h2.offset(), 12345u);
    CHECK_EQ(h2.size(), 4096u);

    std::cout << "  Encoded " << encoded.size() << " bytes ✓\n";
    std::cout << "[PASS] BlockHandle\n";
}

void TestFooter() {
    std::cout << "[TEST] Footer Encode/Decode...\n";

    Footer footer;
    footer.set_metaindex_handle(BlockHandle(100, 200));
    footer.set_index_handle(BlockHandle(300, 400));

    std::string encoded;
    footer.EncodeTo(&encoded);
    CHECK_EQ(encoded.size(), Footer::kEncodedLength); // 必须是 48 字节

    Footer footer2;
    CHECK(footer2.DecodeFrom(Slice(encoded)).Ok());
    CHECK_EQ(footer2.metaindex_handle().offset(), 100u);
    CHECK_EQ(footer2.metaindex_handle().size(), 200u);
    CHECK_EQ(footer2.index_handle().offset(), 300u);
    CHECK_EQ(footer2.index_handle().size(), 400u);

    // 篡改魔数，验证检测
    encoded[Footer::kEncodedLength - 1] ^= 0xFF;
    CHECK(!footer2.DecodeFrom(Slice(encoded)).Ok());

    std::cout << "  Footer = " << Footer::kEncodedLength << " bytes ✓\n";
    std::cout << "  Bad magic detected ✓\n";
    std::cout << "[PASS] Footer\n";
}

// ════════════════════════════════════════════════════════
// TableBuilder 测试
// ════════════════════════════════════════════════════════

void TestTableBuilderSmall() {
    std::cout << "[TEST] TableBuilder Small (< 1 block)...\n";

    std::string path = TestFile("small.ldb");
    Options     opts;
    opts.block_size = 4096;

    {
        TableBuilder builder(path, opts);
        CHECK(builder.status().Ok());

        builder.Add("alpha", "value_a");
        builder.Add("beta", "value_b");
        builder.Add("delta", "value_d");
        builder.Add("gamma", "value_c");

        CHECK_EQ(builder.NumEntries(), 4u);
        Status s = builder.Finish();
        CHECK(s.Ok());
    }

    // 验证文件存在且大小合理
    auto fsize = fs::file_size(path);
    CHECK(fsize > Footer::kEncodedLength); // 至少比 Footer 大

    // 验证文件末尾是合法的 Footer（魔数正确）
    FILE* f = fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    fseek(f, -static_cast<long>(Footer::kEncodedLength), SEEK_END);
    char footer_buf[Footer::kEncodedLength];
    CHECK_EQ(fread(footer_buf, 1, Footer::kEncodedLength, f), Footer::kEncodedLength);
    fclose(f);

    Footer footer;
    CHECK(footer.DecodeFrom(Slice(footer_buf, Footer::kEncodedLength)).Ok());

    std::cout << "  File size=" << fsize << "B, Footer valid ✓\n";
    std::cout << "  index_handle: offset=" << footer.index_handle().offset()
              << ", size=" << footer.index_handle().size() << "\n";
    std::cout << "[PASS] TableBuilder Small\n";
}

void TestTableBuilderMultiBlock() {
    std::cout << "[TEST] TableBuilder Multi DataBlock...\n";

    std::string path = TestFile("multi.ldb");
    Options     opts;
    opts.block_size = 256; // 故意设很小，强制触发多个 DataBlock

    const int N = 200;
    {
        TableBuilder builder(path, opts);

        for (int i = 0; i < N; i++) {
            char key[32], val[64];
            snprintf(key, sizeof(key), "key_%05d", i);
            snprintf(val, sizeof(val), "value_for_key_%05d_padding_xxxxx", i);
            builder.Add(Slice(key), Slice(val));
        }

        CHECK_EQ(builder.NumEntries(), (uint64_t) N);
        CHECK(builder.Finish().Ok());
    }

    // 验证文件有效性：能读到合法 Footer
    FILE* f = fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    fseek(f, -static_cast<long>(Footer::kEncodedLength), SEEK_END);
    char footer_buf[Footer::kEncodedLength];
    fread(footer_buf, 1, Footer::kEncodedLength, f);
    fclose(f);

    Footer footer;
    CHECK(footer.DecodeFrom(Slice(footer_buf, Footer::kEncodedLength)).Ok());

    // Index Block 的 offset 必须在文件合理范围内
    auto fsize = fs::file_size(path);
    CHECK(footer.index_handle().offset() < fsize);
    CHECK(footer.index_handle().offset() > 0u);

    std::cout << "  " << N << " entries, FileSize=" << fsize << "B, IndexBlock@"
              << footer.index_handle().offset() << " ✓\n";
    std::cout << "[PASS] TableBuilder Multi DataBlock\n";
}

void TestTableBuilderFileSize() {
    std::cout << "[TEST] TableBuilder FileSize tracking...\n";

    std::string path = TestFile("size.ldb");
    Options     opts;
    opts.block_size = 4096;

    TableBuilder builder(path, opts);

    const int N = 50;
    for (int i = 0; i < N; i++) {
        char k[16], v[32];
        snprintf(k, sizeof(k), "k%04d", i);
        snprintf(v, sizeof(v), "v%04d_padding_xxx", i);
        builder.Add(Slice(k), Slice(v));
    }

    // Finish 前记录 FileSize（已 flush 的 DataBlock 大小）
    uint64_t before_finish = builder.FileSize();
    auto     res           = builder.Finish();
    CHECK(res.Ok() == true);
    uint64_t after_finish = builder.FileSize();

    // Finish 后文件比 Finish 前大（增加了 Index Block + Footer）
    CHECK(after_finish > before_finish);
    CHECK_EQ(after_finish, fs::file_size(path));

    std::cout << "  BeforeFinish=" << before_finish << "B, AfterFinish=" << after_finish << "B ✓\n";
    std::cout << "[PASS] TableBuilder FileSize\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════

int main() {
    std::cout << "=== minidb Day-06 Tests ===\n\n";

    SetupTestDir();

    // BlockBuilder
    TestBlockBuilderBasic();
    TestBlockBuilderPrefixCompression();
    TestBlockBuilderReset();

    std::cout << "\n";

    // Format
    TestBlockHandle();
    TestFooter();

    std::cout << "\n";

    // TableBuilder
    TestTableBuilderSmall();
    TestTableBuilderMultiBlock();
    TestTableBuilderFileSize();

    CleanupTestDir();

    std::cout << "\n All Day-06 tests passed!\n";
    return 0;
}