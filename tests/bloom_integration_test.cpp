#include "mapledb/db.h"
#include "db/dbformat.h"
#include "table/filter_block.h"
#include "table/table.h"
#include "table/tablebuilder.h"
#include "util/cache.h"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#include <unistd.h>

using namespace mapledb;
namespace fs = std::filesystem;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void OK(const Status& status) {
    if (!status.Ok()) throw std::runtime_error(status.ToString());
}
static std::string Key(int n) {
    char key[32];
    snprintf(key, sizeof key, "key%08d", n);
    return key;
}
static std::string IK(const std::string& key, uint64_t seq, ValueType type = kTypeValue) {
    std::string encoded;
    AppendInternalKey(&encoded, Slice(key), seq, type);
    return encoded;
}
static std::string Contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.good(), "cannot read test SST");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Metadata {
    bool has_filter = false;
    BlockHandle meta, filter;
};
static Metadata GetMetadata(const std::string& bytes) {
    Check(bytes.size() >= Footer::kEncodedLength, "missing footer");
    Footer footer;
    OK(footer.DecodeFrom(Slice(bytes.data() + bytes.size() - Footer::kEncodedLength,
                              Footer::kEncodedLength)));
    Metadata result;
    result.meta = footer.metaindex_handle();
    Block block(bytes.data() + result.meta.offset(), result.meta.size());
    std::unique_ptr<Block::Iter> it(block.NewIterator(BytewiseComparator()));
    it->Seek(Slice(kBloomFilterMetaKey));
    if (it->Valid() && it->key() == Slice(kBloomFilterMetaKey)) {
        Slice value = it->value();
        Check(!value.Empty(), "missing key mode");
        value.RemovePrefix(1);
        OK(result.filter.DecodeFrom(&value));
        Check(value.Empty(), "invalid filter handle");
        result.has_filter = true;
    }
    OK(it->status());
    return result;
}
static void Save(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    Check(file.good(), "cannot write test SST");
}
static void RepairCRC(std::string* bytes, const BlockHandle& handle) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = handle.offset(); i <= handle.offset() + handle.size(); ++i) {
        crc ^= static_cast<unsigned char>((*bytes)[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    crc ^= 0xffffffffu;
    for (int i = 0; i < 4; ++i)
        (*bytes)[handle.offset() + handle.size() + 1 + i] = static_cast<char>(crc >> (8 * i));
}

static void FilterRegionsAndMalformedInput() {
    FilterBlockBuilder builder(10);
    builder.StartBlock(0);
    builder.AddKey(Slice(""));
    builder.AddKey(Slice("alpha"));
    builder.AddKey(Slice("alpha"));
    builder.StartBlock(2048);
    builder.AddKey(Slice("beta"));
    builder.StartBlock(6144); // 4096 对应空区间。
    const std::string binary("\0x\xff", 3);
    builder.AddKey(Slice(binary));
    const std::string encoded = builder.Finish().ToString();
    FilterBlockReader reader{Slice(encoded)};
    Check(reader.KeyMayMatch(0, Slice("")), "empty key rejected");
    Check(reader.KeyMayMatch(100, Slice("alpha")), "first region key rejected");
    Check(reader.KeyMayMatch(2048, Slice("beta")), "second region key rejected");
    Check(!reader.KeyMayMatch(4096, Slice("anything")), "empty region not rejected");
    Check(reader.KeyMayMatch(6144, Slice(binary)), "binary key rejected");
    Check(reader.KeyMayMatch(8192, Slice("unknown")), "unknown region must fall back");
    for (size_t n = 0; n < 5; ++n) {
        FilterBlockReader malformed{Slice(encoded.data(), n)};
        Check(malformed.KeyMayMatch(0, Slice("alpha")), "short filter rejected a key");
    }
    std::string malformed = encoded;
    malformed.back() = static_cast<char>(255);
    Check(FilterBlockReader{Slice(malformed)}.KeyMayMatch(0, Slice("alpha")),
          "unknown format did not fall back");
    malformed = encoded;
    const uint32_t array = DecodeFixed32(malformed.data() + malformed.size() - 5);
    malformed[array] = static_cast<char>(255); // 非零首偏移且超出过滤器内容。
    Check(FilterBlockReader{Slice(malformed)}.KeyMayMatch(0, Slice("alpha")),
          "bad offsets did not fall back");
    Check(BloomFilter(10).KeyMayMatch(Slice("alpha"), Slice("")),
          "malformed Bloom bits did not fall back");
}

static constexpr int kKeys = 1024;
static void BuildRaw(const fs::path& path, int bits, const Comparator* cmp = BytewiseComparator()) {
    Options opts;
    opts.block_size = 64;
    opts.bloom_bits_per_key = bits;
    TableBuilder builder(path.string(), opts, cmp);
    for (int i = 0; i < kKeys; ++i) builder.Add(Slice(Key(i * 2)), Slice(std::string(96, 'v')));
    OK(builder.Finish());
}
static void VerifyRaw(const fs::path& path, int bits = 10) {
    Options opts;
    opts.bloom_bits_per_key = bits;
    Table* opened = nullptr;
    OK(Table::Open(path.string(), opts, &opened));
    std::unique_ptr<Table> table(opened);
    for (int i = 0; i < kKeys; ++i) {
        struct Result { bool found = false; std::string key, value; } result;
        OK(table->InternalGet(Slice(Key(i * 2)), &result,
            [](void* arg, const Slice& key, const Slice& value) {
                auto* result = static_cast<Result*>(arg);
                result->found = true; result->key = key.ToString(); result->value = value.ToString();
            }));
        Check(result.found && result.key == Key(i * 2) && result.value == std::string(96, 'v'),
              "raw table produced a false negative");
    }
    std::unique_ptr<Iterator> it(table->NewIterator());
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++count)
        Check(it->key().ToString() == Key(count * 2), "scan skipped or reordered a key");
    OK(it->status());
    Check(count == kKeys, "scan key count changed");
}
static size_t MissingReadCharge(const fs::path& path, int bits) {
    std::unique_ptr<Cache> cache(NewLRUCache(8 * 1024 * 1024));
    Options opts;
    opts.block_cache = cache.get(); opts.bloom_bits_per_key = bits;
    Table* opened = nullptr;
    OK(Table::Open(path.string(), opts, &opened));
    std::unique_ptr<Table> table(opened);
    Check(cache->TotalCharge() == 0, "metadata entered DataBlock cache");
    for (int i = 0; i < kKeys - 1; ++i)
        OK(table->InternalGet(Slice(Key(i * 2 + 1)), nullptr, [](void*, const Slice&, const Slice&) {}));
    return cache->TotalCharge();
}

static void PersistedFilterAndFallback(const fs::path& root) {
    const auto filtered = root / "filtered.ldb", legacy = root / "legacy.ldb";
    BuildRaw(filtered, 10);
    BuildRaw(legacy, 0);
    Check(GetMetadata(Contents(filtered)).has_filter, "filter was not persisted");
    Check(!GetMetadata(Contents(legacy)).has_filter, "disabled filter was persisted");
    VerifyRaw(filtered); VerifyRaw(filtered, 0); VerifyRaw(filtered, 20); VerifyRaw(legacy);
    const auto enabled = MissingReadCharge(filtered, 10), disabled = MissingReadCharge(filtered, 0);
    Check(disabled > 0 && enabled * 4 < disabled, "filter did not skip DataBlock reads");
    std::cout << "Missing-query DataBlock cache charge: enabled=" << enabled << ", disabled=" << disabled << '\n';

    const std::string original = Contents(filtered);
    const Metadata metadata = GetMetadata(original);
    std::string corrupt = original;
    corrupt[metadata.filter.offset()] ^= 1; // CRC 不匹配，必须忽略整个过滤器。
    const auto damaged = root / "damaged.ldb";
    Save(damaged, corrupt); VerifyRaw(damaged);
    Check(MissingReadCharge(damaged, 10) == disabled, "bad filter CRC was still used");

    corrupt = original;
    const auto array_pos = metadata.filter.offset() + metadata.filter.size() - 5;
    for (int i = 0; i < 4; ++i) corrupt[array_pos + i] = static_cast<char>(255);
    RepairCRC(&corrupt, metadata.filter); // CRC 正确、结构损坏，也必须回退。
    Save(damaged, corrupt); VerifyRaw(damaged);
    Check(MissingReadCharge(damaged, 10) == disabled, "malformed offsets were still used");

    corrupt = original;
    corrupt[metadata.meta.offset()] ^= 1;
    Save(damaged, corrupt); VerifyRaw(damaged);
    Check(MissingReadCharge(damaged, 10) == disabled, "bad MetaIndex CRC was still used");

    class UnknownComparator final : public BytewiseComparatorImpl {} comparator;
    const auto custom = root / "custom.ldb";
    BuildRaw(custom, 10, &comparator);
    Check(!GetMetadata(Contents(custom)).has_filter, "unknown comparator used byte hashing");
    VerifyRaw(custom);
}

static void InternalKeysAndSequences(const fs::path& root) {
    Options opts;
    opts.block_size = 32;
    InternalKeyComparator cmp;
    const auto path = root / "internal.ldb";
    std::vector<std::string> users{"", std::string("\0key", 4), "a", "ab", std::string("a\0b", 3)};
    for (int i = 0; i < 300; ++i) users.push_back(Key(i));
    std::sort(users.begin(), users.end()); // 含 NUL 的 key 也必须满足实际字节序。
    {
        TableBuilder builder(path.string(), opts, &cmp);
        for (const auto& user : users)
            for (uint64_t seq : {300, 200, 100})
                builder.Add(Slice(IK(user, seq)), Slice(std::to_string(seq)));
        OK(builder.Finish());
    }
    Check(GetMetadata(Contents(path)).has_filter, "internal filter missing");
    Table* opened = nullptr;
    OK(Table::Open(path.string(), opts, &cmp, &opened));
    std::unique_ptr<Table> table(opened);
    for (const auto& user : users) for (uint64_t seq : {400, 250, 150}) {
        struct Result { bool found = false; std::string key, value; } result;
        OK(table->InternalGet(Slice(IK(user, seq)), &result,
            [](void* arg, const Slice& key, const Slice& value) {
                auto* r = static_cast<Result*>(arg);
                r->found = true; r->key = key.ToString(); r->value = value.ToString();
            }));
        const uint64_t expected = seq == 400 ? 300 : seq == 250 ? 200 : 100;
        Check(result.found && result.key == IK(user, expected) && result.value == std::to_string(expected),
              "query sequence caused Bloom false negative");
    }
}

static std::unique_ptr<DB> Open(const fs::path& path, const Options& opts) {
    DB* db = nullptr;
    OK(DB::Open(opts, path.string(), &db));
    return std::unique_ptr<DB>(db);
}
static void VerifyDB(DB* db, const std::map<std::string, std::string>& expected, int n) {
    for (int i = 0; i < n; ++i) {
        const auto key = Key(i);
        std::string value;
        const auto status = db->Get(key, &value);
        const auto it = expected.find(key);
        if (it == expected.end()) Check(status.IsNotFound(), "missing/deleted DB key reappeared");
        else { OK(status); Check(value == it->second, "incorrect DB value with Bloom Filter"); }
    }
    std::unique_ptr<Iterator> it(db->NewIterator());
    it->SeekToFirst();
    for (const auto& entry : expected) {
        Check(it->Valid() && it->key().ToString() == entry.first && it->value().ToString() == entry.second,
              "DB scan changed with Bloom Filter");
        it->Next();
    }
    Check(!it->Valid(), "DB scan returned extra keys");
    OK(it->status());
}
static void DBRecoveryAndCompaction(const fs::path& root) {
    const auto path = root / "mixed-db";
    Options opts;
    opts.bloom_bits_per_key = 0;
    opts.writer_buffer_size = 32 * 1024;
    opts.max_file_size = 32 * 1024;
    std::map<std::string, std::string> expected;
    {
        auto db = Open(path, opts);
        for (int i = 0; i < 400; ++i) { expected[Key(i)] = std::string(96, 'a'); OK(db->Put(Key(i), expected[Key(i)])); }
        OK(db->Flush());
    }
    opts.bloom_bits_per_key = 10;
    opts.l0_compaction_trigger = 2;
    {
        auto db = Open(path, opts);
        VerifyDB(db.get(), expected, 400);
        for (int i = 0; i < 1200; ++i) {
            expected[Key(i)] = std::string(96, 'b'); OK(db->Put(Key(i), expected[Key(i)]));
        }
        for (int i = 0; i < 1200; i += 3) { OK(db->Delete(Key(i))); expected.erase(Key(i)); }
        OK(db->Flush());
        VerifyDB(db.get(), expected, 1400);
    }
    size_t filters = 0;
    for (const auto& file : fs::directory_iterator(path)) if (file.path().extension() == ".ldb") {
        Check(GetMetadata(Contents(file.path())).has_filter, "Flush/Compaction output missing filter");
        ++filters;
    }
    Check(filters > 0, "no compaction SST outputs");
    for (int bits : {10, 0, 20}) {
        opts.bloom_bits_per_key = bits;
        auto db = Open(path, opts);
        VerifyDB(db.get(), expected, 1400);
    }

    opts.bloom_bits_per_key = 10;
    opts.l0_compaction_trigger = 100;
    const auto deleted = root / "tombstone-db";
    const std::vector<std::string> users{"", "shadow", std::string("a\0b", 3)};
    {
        auto db = Open(deleted, opts);
        for (const auto& user : users) OK(db->Put(user, "old"));
        OK(db->Flush());
        for (const auto& user : users) OK(db->Delete(user));
        OK(db->Flush()); // 独立 L0 删除表的过滤器必须包含 tombstone 的 user key。
    }
    auto db = Open(deleted, opts);
    for (const auto& user : users) {
        std::string value;
        Check(db->Get(user, &value).IsNotFound(), "tombstone filter resurrected an older value");
    }
}

int main() {
    const auto root = fs::temp_directory_path() / ("mapledb-bloom-" + std::to_string(getpid()));
    try {
        Check(fs::create_directory(root), "test directory already exists");
        FilterRegionsAndMalformedInput();
        PersistedFilterAndFallback(root);
        InternalKeysAndSequences(root);
        DBRecoveryAndCompaction(root);
        fs::remove_all(root);
        std::cout << "Bloom integration tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << " (data: " << root << ")\n";
        return 1;
    }
}
