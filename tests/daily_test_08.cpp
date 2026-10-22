// tests/day08_test.cpp
// 第8天测试：VersionEdit 序列化/反序列化 + VersionSet 核心流程

#include "db/dbformat.h"
#include "db/versionedit.h"
#include "db/versionset.h"
#include "mapledb/options.h"

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
#define CHECK_STR_EQ(a, b) CHECK_EQ(std::string(a), std::string(b))

static const std::string kTestDir = "/tmp/minidb_day08_test";
static void              SetupTestDir() { fs::create_directories(kTestDir); }
static void              CleanupTestDir() { fs::remove_all(kTestDir); }
static std::string       SubDir(const std::string& n) {
    std::string d = kTestDir + "/" + n;
    fs::remove_all(d); // ← 加这行：先清空
    fs::create_directories(d);
    return d;
}

// 修复：元数据边界和查找参数都要求二进制 InternalKey；普通字符串或 "key@seq:type"
// 只是可读文本，缺少合法的 8 字节 tag，会被格式校验拒绝或导致比较器断言。
static std::string EncodeKey(const std::string& key, SequenceNumber sequence = 1) {
    std::string encoded;
    AppendInternalKey(&encoded, Slice(key), sequence, kTypeValue);
    return encoded;
}

// ════════════════════════════════════════════════════════
// VersionEdit 测试
// ════════════════════════════════════════════════════════

void TestVersionEditEncodeDecode() {
    std::cout << "[TEST] VersionEdit EncodeTo / DecodeFrom...\n";

    VersionEdit edit;
    // 修复：使用生产 MANIFEST 快照写入的比较器名称，保持格式契约一致。
    edit.SetComparatorName(Slice("leveldb.BytewiseComparator"));
    edit.SetLogNumber(5);
    edit.SetNextFile(10);
    edit.SetLastSequence(1000);

    // 添加两个文件
    edit.AddFile(0, 3, 4096, EncodeKey("apple", 5), EncodeKey("cherry", 3));
    edit.AddFile(1, 1, 8192, EncodeKey("date", 2), EncodeKey("grape", 1));

    // 删除一个文件
    edit.DeleteFile(1, 7);

    // 序列化
    std::string encoded;
    edit.EncodeTo(&encoded);
    CHECK(!encoded.empty());

    // 反序列化
    VersionEdit decoded;
    Status      s = decoded.DecodeFrom(Slice(encoded));
    CHECK(s.Ok());

    // 验证各字段
    CHECK(decoded.has_comparator());
    CHECK_STR_EQ(decoded.comparator(), "leveldb.BytewiseComparator");

    CHECK(decoded.has_log_number());
    CHECK_EQ(decoded.log_number(), 5u);

    CHECK(decoded.has_next_file_number());
    CHECK_EQ(decoded.next_file_number(), 10u);

    CHECK(decoded.has_last_sequence());
    CHECK_EQ(decoded.last_sequence(), 1000u);

    // 验证新增文件
    const auto& nf = decoded.new_files();
    CHECK_EQ(nf.size(), 2u);
    CHECK_EQ(nf[0].first, 0); // level 0
    CHECK_EQ(nf[0].second.number, 3u);
    CHECK_EQ(nf[0].second.file_size, 4096u);
    CHECK_STR_EQ(nf[0].second.smallest, EncodeKey("apple", 5));
    CHECK_STR_EQ(nf[0].second.largest, EncodeKey("cherry", 3));

    // 验证删除文件
    const auto& df = decoded.deleted_files();
    CHECK_EQ(df.size(), 1u);
    CHECK(df.count({1, 7}) == 1);

    std::cout << "  Encoded " << encoded.size() << " bytes ✓\n";
    std::cout << "  2 new files + 1 deleted file decoded correctly ✓\n";
    std::cout << "[PASS] VersionEdit EncodeTo / DecodeFrom\n";
}

void TestVersionEditEmpty() {
    std::cout << "[TEST] VersionEdit Empty...\n";

    VersionEdit edit;
    std::string encoded;
    edit.EncodeTo(&encoded);
    // 空 VersionEdit 序列化结果为空字符串（没有任何 tag）
    CHECK(encoded.empty());

    VersionEdit decoded;
    CHECK(decoded.DecodeFrom(Slice(encoded)).Ok());
    CHECK(!decoded.has_comparator());
    CHECK(!decoded.has_log_number());
    CHECK(decoded.new_files().empty());
    CHECK(decoded.deleted_files().empty());

    std::cout << "  Empty VersionEdit: encode/decode OK ✓\n";
    std::cout << "[PASS] VersionEdit Empty\n";
}

// ════════════════════════════════════════════════════════
// VersionSet 测试
// ════════════════════════════════════════════════════════

void TestVersionSetBasicAddFile() {
    std::cout << "[TEST] VersionSet AddFile...\n";

    std::string dbdir = SubDir("add_file");
    Options     opts;
    VersionSet  vs(dbdir, opts);

    CHECK_EQ(vs.NumFiles(0), 0);
    CHECK_EQ(vs.NumFiles(1), 0);

    // 添加文件到 Level 0
    VersionEdit edit;
    edit.SetLogNumber(1);
    edit.SetNextFile(10);
    edit.SetLastSequence(100);
    edit.AddFile(0, 3, 4096, EncodeKey("apple"), EncodeKey("cherry"));
    edit.AddFile(0, 4, 4096, EncodeKey("date"), EncodeKey("grape"));

    Status s = vs.LogAndApply(&edit);
    CHECK(s.Ok());

    CHECK_EQ(vs.NumFiles(0), 2);
    CHECK_EQ(vs.NumFiles(1), 0);

    // 文件应按 smallest key 排序
    const auto& files = vs.GetFiles(0);
    CHECK_EQ(files[0]->number, 3u); // "apple" < "date"
    CHECK_EQ(files[1]->number, 4u);

    std::cout << "  Added 2 files to L0, sorted by smallest key ✓\n";
    std::cout << "[PASS] VersionSet AddFile\n";
}

void TestVersionSetDeleteFile() {
    std::cout << "[TEST] VersionSet DeleteFile...\n";

    std::string dbdir = SubDir("del_file");
    Options     opts;
    VersionSet  vs(dbdir, opts);

    // 先添加
    {
        VersionEdit e;
        e.SetLastSequence(10);
        e.SetNextFile(5);
        e.AddFile(1, 1, 8192, EncodeKey("aaa"), EncodeKey("mmm"));
        e.AddFile(1, 2, 8192, EncodeKey("nnn"), EncodeKey("zzz"));
        CHECK(vs.LogAndApply(&e).Ok());
    }
    CHECK_EQ(vs.NumFiles(1), 2);

    // 再删除 file#1
    {
        VersionEdit e;
        e.SetLastSequence(20);
        e.SetNextFile(6);
        e.DeleteFile(1, 1);
        CHECK(vs.LogAndApply(&e).Ok());
    }
    CHECK_EQ(vs.NumFiles(1), 1);
    CHECK_EQ(vs.GetFiles(1)[0]->number, 2u);

    std::cout << "  Added 2, deleted 1, 1 remains ✓\n";
    std::cout << "[PASS] VersionSet DeleteFile\n";
}

void TestVersionSetFileNumber() {
    std::cout << "[TEST] VersionSet NewFileNumber...\n";

    std::string dbdir = SubDir("file_num");
    Options     opts;
    VersionSet  vs(dbdir, opts);

    // 初始 next_file_number_ = 2
    uint64_t n1 = vs.NewFileNumber();
    uint64_t n2 = vs.NewFileNumber();
    uint64_t n3 = vs.NewFileNumber();

    CHECK_EQ(n1, 2u);
    CHECK_EQ(n2, 3u);
    CHECK_EQ(n3, 4u);

    std::cout << "  Allocated: " << n1 << ", " << n2 << ", " << n3 << " ✓\n";
    std::cout << "[PASS] VersionSet NewFileNumber\n";
}

// ════════════════════════════════════════════════════════
// Manifest 持久化与恢复测试（核心）
// ════════════════════════════════════════════════════════

void TestVersionSetRecover() {
    std::cout << "[TEST] VersionSet Recover (restart simulation)...\n";

    std::string dbdir = SubDir("recover");

    // ── 第一次运行：写入若干 VersionEdit ───────────────────────
    {
        Options    opts;
        VersionSet vs(dbdir, opts);

        // Edit 1: 添加 2 个 L0 文件
        {
            VersionEdit e;
            e.SetLogNumber(1);
            e.SetNextFile(5);
            e.SetLastSequence(100);
            e.AddFile(0, 3, 4096, EncodeKey("apple"), EncodeKey("cherry"));
            e.AddFile(0, 4, 4096, EncodeKey("date"), EncodeKey("grape"));
            CHECK(vs.LogAndApply(&e).Ok());
        }
        // Edit 2: 从 L0 compact 到 L1（删除 L0 文件，添加 L1 文件）
        {
            VersionEdit e;
            e.SetLogNumber(2);
            e.SetNextFile(6);
            e.SetLastSequence(200);
            e.DeleteFile(0, 3);
            e.DeleteFile(0, 4);
            e.AddFile(1, 5, 8192, EncodeKey("apple"), EncodeKey("grape"));
            CHECK(vs.LogAndApply(&e).Ok());
        }

        // 验证最终状态
        CHECK_EQ(vs.NumFiles(0), 0);
        CHECK_EQ(vs.NumFiles(1), 1);
        CHECK_EQ(vs.GetFiles(1)[0]->number, 5u);
        std::cout << "  First run: L0=0 files, L1=1 file ✓\n";
    }
    // vs 析构，模拟进程退出

    // ── 第二次运行：从 MANIFEST 恢复 ─────────────────────────
    {
        Options    opts;
        VersionSet vs2(dbdir, opts);

        Status s = vs2.Recover();
        CHECK(s.Ok());

        // 验证恢复结果
        CHECK_EQ(vs2.NumFiles(0), 0);
        CHECK_EQ(vs2.NumFiles(1), 1);
        CHECK_EQ(vs2.GetFiles(1)[0]->number, 5u);
        CHECK_EQ(vs2.GetFiles(1)[0]->file_size, 8192u);
        CHECK_STR_EQ(vs2.GetFiles(1)[0]->smallest, EncodeKey("apple"));
        CHECK_STR_EQ(vs2.GetFiles(1)[0]->largest, EncodeKey("grape"));
        CHECK_EQ(vs2.LastSequence(), 200u);

        std::cout << "  Recovery: L0=0, L1=1 (file#5) ✓\n";
        // 内部键包含二进制 tag，输出时仅展示用户键，避免控制字符混入日志。
        std::cout << "  smallest=" << ExtractUserKey(vs2.GetFiles(1)[0]->smallest).ToString()
                  << ", largest=" << ExtractUserKey(vs2.GetFiles(1)[0]->largest).ToString() << " ✓\n";
        std::cout << "  LastSequence=" << vs2.LastSequence() << " ✓\n";
    }

    std::cout << "[PASS] VersionSet Recover\n";
}

void TestVersionSetMultiLevelRecover() {
    std::cout << "[TEST] VersionSet Multi-Level Recover...\n";

    std::string dbdir = SubDir("multi_level");

    // 构造一个多层文件的场景
    {
        Options    opts;
        VersionSet vs(dbdir, opts);

        VersionEdit e;
        e.SetLogNumber(5);
        e.SetNextFile(20);
        e.SetLastSequence(5000);
        // L0: 1 个文件
        e.AddFile(0, 10, 1024, EncodeKey("key_0001"), EncodeKey("key_0100"));
        // L1: 2 个文件
        e.AddFile(1, 11, 4096, EncodeKey("key_0101"), EncodeKey("key_0200"));
        e.AddFile(1, 12, 4096, EncodeKey("key_0201"), EncodeKey("key_0300"));
        // L2: 1 个文件
        e.AddFile(2, 13, 8192, EncodeKey("key_0001"), EncodeKey("key_0300"));

        CHECK(vs.LogAndApply(&e).Ok());

        std::cout << "  Before recovery: L0=" << vs.NumFiles(0) << " L1=" << vs.NumFiles(1)
                  << " L2=" << vs.NumFiles(2) << "\n";
    }

    // 重启恢复
    {
        Options    opts;
        VersionSet vs2(dbdir, opts);
        CHECK(vs2.Recover().Ok());

        CHECK_EQ(vs2.NumFiles(0), 1);
        CHECK_EQ(vs2.NumFiles(1), 2);
        CHECK_EQ(vs2.NumFiles(2), 1);
        CHECK_EQ(vs2.LastSequence(), 5000u);
        CHECK_EQ(vs2.TotalFileSize(), (uint64_t) (1024 + 4096 + 4096 + 8192));

        std::cout << "  After recovery: L0=" << vs2.NumFiles(0) << " L1=" << vs2.NumFiles(1)
                  << " L2=" << vs2.NumFiles(2) << " ✓\n";
        std::cout << "  TotalFileSize=" << vs2.TotalFileSize() << "B ✓\n";
    }

    std::cout << "[PASS] VersionSet Multi-Level Recover\n";
}

void TestVersionSetFindFile() {
    std::cout << "[TEST] VersionSet FindFile (binary search)...\n";

    // 构造一组有序文件（Level 1+ 无重叠）
    std::vector<FileMetaData*> files;
    auto make_file = [&](uint64_t num, const std::string& s, const std::string& l) {
        FileMetaData* f = new FileMetaData();
        f->number       = num;
        f->smallest     = EncodeKey(s);
        f->largest      = EncodeKey(l);
        files.push_back(f);
    };

    make_file(1, "aaa", "ccc");
    make_file(2, "ddd", "fff");
    make_file(3, "ggg", "iii");
    make_file(4, "jjj", "lll");

    // FindFile 找第一个 largest >= key 的文件索引
    CHECK_EQ(VersionSet::FindFile(files, Slice(EncodeKey("aaa"))), 0); // 在 file#1 里
    CHECK_EQ(VersionSet::FindFile(files, Slice(EncodeKey("ccc"))), 0); // 在 file#1 末尾
    CHECK_EQ(VersionSet::FindFile(files, Slice(EncodeKey("ddd"))), 1); // 在 file#2 开头
    CHECK_EQ(VersionSet::FindFile(files, Slice(EncodeKey("eee"))), 1); // 在 file#2 里
    CHECK_EQ(VersionSet::FindFile(files, Slice(EncodeKey("zzz"))), 4); // 超出所有文件

    // 清理
    for (FileMetaData* f : files)
        delete f;

    std::cout << "  FindFile binary search: all cases correct ✓\n";
    std::cout << "[PASS] VersionSet FindFile\n";
}

// ════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════

int main() {
    std::cout << "=== minidb Day-08 Tests ===\n\n";

    SetupTestDir();

    // VersionEdit
    TestVersionEditEncodeDecode();
    TestVersionEditEmpty();

    std::cout << "\n";

    // VersionSet
    TestVersionSetBasicAddFile();
    TestVersionSetDeleteFile();
    TestVersionSetFileNumber();

    std::cout << "\n";

    // Manifest 持久化与恢复（核心）
    TestVersionSetRecover();
    TestVersionSetMultiLevelRecover();
    TestVersionSetFindFile();

    CleanupTestDir();

    std::cout << "\n All Day-08 tests passed!\n";
    return 0;
}