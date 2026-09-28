// One request generator and timed loop for both engines. No db_bench workloads are reused.
#include "mapledb/db.h"
#include "util/cache.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/iterator.h"
#include "db/db_impl.h" // Pinned LevelDB 1.23: flush without forcing full-tree compaction.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr size_t kWriteBuffer = 4 * 1024 * 1024;
constexpr size_t kBlockSize = 4096;
constexpr size_t kFileSize = 2 * 1024 * 1024;
constexpr size_t kLatencyStride = 257;

static void Require(const mapledb::Status& s) {
    if (!s.Ok()) throw std::runtime_error(s.ToString());
}
static void Require(const leveldb::Status& s) {
    if (!s.ok()) throw std::runtime_error(s.ToString());
}

// Counts queued + running jobs; the timed write path remains the upstream implementation.
class DrainEnv final : public leveldb::EnvWrapper {
public:
    DrainEnv() : EnvWrapper(leveldb::Env::Default()) {}
    ~DrainEnv() override { WaitIdle(); }
    void Schedule(void (*function)(void*), void* arg) override {
        auto* task = new Task{this, function, arg};
        { std::lock_guard<std::mutex> lock(mutex_); ++pending_; }
        target()->Schedule([](void* p) {
            std::unique_ptr<Task> task(static_cast<Task*>(p));
            task->function(task->arg);
            std::lock_guard<std::mutex> lock(task->env->mutex_);
            --task->env->pending_;
            task->env->cv_.notify_all();
        }, task);
    }
    void WaitIdle() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return pending_ == 0; });
    }
private:
    struct Task { DrainEnv* env; void (*function)(void*); void* arg; };
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t pending_ = 0;
};

struct MapleCursor {
    std::unique_ptr<mapledb::Iterator> iterator;
    void First() { iterator->SeekToFirst(); }
    bool Valid() const { return iterator->Valid(); }
    void Next() { iterator->Next(); }
    std::string_view Key() const { auto& s = iterator->key(); return {s.Data(), s.Size()}; }
    std::string_view Value() const { auto& s = iterator->value(); return {s.Data(), s.Size()}; }
    void Check() const { Require(iterator->status()); }
};
struct LevelCursor {
    std::unique_ptr<leveldb::Iterator> iterator;
    void First() { iterator->SeekToFirst(); }
    bool Valid() const { return iterator->Valid(); }
    void Next() { iterator->Next(); }
    std::string_view Key() const { auto s = iterator->key(); return {s.data(), s.size()}; }
    std::string_view Value() const { auto s = iterator->value(); return {s.data(), s.size()}; }
    void Check() const { Require(iterator->status()); }
};

class MapleEngine {
public:
    MapleEngine(const fs::path& path, size_t cache_bytes, int bloom_bits)
        : cache_(mapledb::NewLRUCache(cache_bytes)) {
        mapledb::Options opts;
        opts.writer_buffer_size = kWriteBuffer;
        opts.block_size = kBlockSize;
        opts.max_file_size = kFileSize;
        opts.block_restart_interval = 16;
        opts.block_cache = cache_.get();
        opts.bloom_bits_per_key = bloom_bits; // 基线默认关闭；与 LevelDB 的开关一致。
        mapledb::DB* db = nullptr;
        Require(mapledb::DB::Open(opts, path.string(), &db));
        db_.reset(db);
    }
    void Put(const std::string& k, const std::string& v) { Require(db_->Put(k, v)); }
    bool Get(const std::string& k, std::string* v) {
        auto s = db_->Get(k, v);
        if (s.IsNotFound()) return false;
        Require(s); return true;
    }
    void Drain() { Require(db_->Flush()); }
    MapleCursor NewCursor() { return {std::unique_ptr<mapledb::Iterator>(db_->NewIterator())}; }
private:
    std::unique_ptr<mapledb::Cache> cache_;
    std::unique_ptr<mapledb::DB> db_;
};

class LevelEngine {
public:
    LevelEngine(const fs::path& path, size_t cache_bytes, int bloom_bits)
        : cache_(leveldb::NewLRUCache(cache_bytes)),
          filter_(bloom_bits > 0 ? leveldb::NewBloomFilterPolicy(bloom_bits) : nullptr) {
        leveldb::Options opts;
        opts.create_if_missing = true;
        opts.write_buffer_size = kWriteBuffer;
        opts.block_size = kBlockSize;
        opts.max_file_size = kFileSize;
        opts.block_restart_interval = 16;
        opts.block_cache = cache_.get();
        opts.max_open_files = 110; // 100 TableCache entries + 10 non-table files.
        opts.compression = leveldb::kNoCompression;
        opts.filter_policy = filter_.get();
        opts.env = &env_;
        leveldb::DB* db = nullptr;
        Require(leveldb::DB::Open(opts, path.string(), &db));
        db_.reset(db);
    }
    void Put(const std::string& k, const std::string& v) {
        Require(db_->Put(leveldb::WriteOptions(), k, v)); // sync=false, batch size=1.
    }
    bool Get(const std::string& k, std::string* v) {
        auto s = db_->Get(leveldb::ReadOptions(), k, v);
        if (s.IsNotFound()) return false;
        Require(s); return true;
    }
    void Drain() {
        Require(static_cast<leveldb::DBImpl*>(db_.get())->TEST_CompactMemTable());
        env_.WaitIdle();
    }
    LevelCursor NewCursor() { return {std::unique_ptr<leveldb::Iterator>(db_->NewIterator(leveldb::ReadOptions()))}; }
private:
    DrainEnv env_;
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> filter_;
    std::unique_ptr<leveldb::DB> db_;
};

static uint64_t Mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static uint64_t Hash(uint64_t h, std::string_view bytes) {
    for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static uint64_t OrderHash(const std::vector<size_t>& order) {
    uint64_t h = 14695981039346656037ULL;
    for (uint64_t i : order) for (int b = 0; b < 8; ++b) {
        h ^= static_cast<unsigned char>(i >> (8 * b)); h *= 1099511628211ULL;
    }
    return h;
}
static std::string MakeKey(uint64_t i) {
    char b[32];
    snprintf(b, sizeof b, "%016llu", static_cast<unsigned long long>(i));
    return b;
}
struct Dataset {
    std::vector<std::string> keys, values, missing;
    std::vector<size_t> sequential, random, hot;
    uint64_t hash = 14695981039346656037ULL;
    uint64_t random_hash, hot_hash;
    Dataset(size_t n, size_t value_bytes, uint64_t seed)
        : keys(n), values(n), missing(n), sequential(n), random(n), hot(n) {
        for (size_t i = 0; i < n; ++i) {
            keys[i] = MakeKey(i * 2);
            missing[i] = MakeKey(i * 2 + 1); // Interleaved misses exercise SST lookup, not just range rejection.
            values[i].resize(value_bytes);
            uint64_t bits = 0;
            for (size_t b = 0; b < value_bytes; ++b) {
                if (b % 8 == 0) bits = Mix(seed ^ (i * 0x9e3779b97f4a7c15ULL) ^ b);
                values[i][b] = static_cast<char>(bits >> (8 * (b % 8)));
            }
            hash = Hash(Hash(hash, keys[i]), values[i]);
        }
        std::iota(sequential.begin(), sequential.end(), 0);
        random = sequential;
        std::mt19937_64 rng(seed);
        std::shuffle(random.begin(), random.end(), rng);
        const size_t hot_count = std::max<size_t>(1, n / 100);
        for (auto& i : hot) i = rng() % hot_count;
        random_hash = OrderHash(random);
        hot_hash = OrderHash(hot);
    }
};

struct Result {
    std::string workload;
    double ms = 0, settle_ms = 0;
    double p50_ns = 0, p95_ns = 0, p99_ns = 0;
    uint64_t checksum = 0;
    size_t sst_files = 0;
    uint64_t sst_bytes = 0;
};
static double Milliseconds(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}
template<class Operation>
static Result Measure(const std::string& name, size_t n, Operation operation) {
    Result r;
    r.workload = name;
    std::vector<double> samples;
    samples.reserve((n + kLatencyStride - 1) / kLatencyStride);
    const auto start = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        if (i % kLatencyStride == 0) {
            const auto a = Clock::now();
            r.checksum += operation(i);
            samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - a).count());
        } else r.checksum += operation(i);
    }
    r.ms = Milliseconds(Clock::now() - start);
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double p) { return samples[static_cast<size_t>(p * (samples.size() - 1))]; };
    r.p50_ns = percentile(0.50); r.p95_ns = percentile(0.95); r.p99_ns = percentile(0.99);
    return r;
}
static void FileStats(const fs::path& path, Result* r) {
    for (const auto& entry : fs::directory_iterator(path))
        if (entry.path().extension() == ".ldb" || entry.path().extension() == ".sst") {
            ++r->sst_files; r->sst_bytes += entry.file_size();
        }
}
template<class Engine>
static uint64_t Read(Engine& engine, const Dataset& data, const std::vector<size_t>& order,
                     size_t position, bool missing, std::string& value) {
    const auto id = order[position];
    const bool found = engine.Get(missing ? data.missing[id] : data.keys[id], &value);
    if (missing) {
        if (found) throw std::runtime_error("missing key unexpectedly found");
        return 0;
    }
    if (!found || value != data.values[id]) throw std::runtime_error("point read validation failed");
    return value.size() + static_cast<unsigned char>(value.front()) + static_cast<unsigned char>(value.back());
}
template<class Engine>
static Result Scan(Engine& engine, const Dataset& data, bool timed) {
    const auto start = Clock::now();
    auto cursor = engine.NewCursor();
    cursor.First();
    const auto n = data.keys.size();
    auto r = Measure("scan", n, [&](size_t i) -> uint64_t {
        if (!cursor.Valid() || cursor.Key() != data.keys[i] || cursor.Value() != data.values[i])
            throw std::runtime_error("scan order/value/count validation failed");
        const uint64_t checksum = cursor.Value().size() + static_cast<unsigned char>(cursor.Value().front()) +
                                  static_cast<unsigned char>(cursor.Value().back());
        cursor.Next(); return checksum;
    });
    if (cursor.Valid()) throw std::runtime_error("scan returned extra keys");
    cursor.Check();
    if (timed) r.ms = Milliseconds(Clock::now() - start); // Includes iterator creation and final status.
    return r;
}

template<class Engine>
static std::vector<Result> Run(const fs::path& root, const Dataset& data, size_t cache_bytes,
                               int bloom_bits) {
    std::vector<Result> results;
    const size_t n = data.keys.size();
    for (bool random : {false, true}) {
        const auto path = root / (random ? "random" : "sequential");
        if (!fs::create_directory(path)) throw std::runtime_error("benchmark DB directory already exists");
        Engine engine(path, cache_bytes, bloom_bits);
        const auto& order = random ? data.random : data.sequential;
        auto r = Measure(random ? "write_random" : "write_sequential", n, [&](size_t p) -> uint64_t {
            const auto i = order[p]; engine.Put(data.keys[i], data.values[i]); return 1;
        });
        const auto drain_start = Clock::now();
        engine.Drain();
        r.settle_ms = Milliseconds(Clock::now() - drain_start);
        FileStats(path, &r);
        results.push_back(r);
        Scan(engine, data, false); // Full verification outside write timing.
    }
    // Reads all use the same random-unique-write dataset. Each workload reopens with a fresh cache.
    const auto path = root / "random";
    for (const std::string name : {"get_sequential", "get_random", "get_hot", "get_missing", "scan"}) {
        Engine engine(path, cache_bytes, bloom_bits);
        const auto& order = name == "get_random" ? data.random : name == "get_hot" ? data.hot : data.sequential;
        const bool missing = name == "get_missing";
        Result r;
        std::string value;
        value.reserve(data.values.front().size());
        if (name == "scan") {
            Scan(engine, data, false);
            r = Scan(engine, data, true);
        } else {
            for (size_t p = 0; p < n; ++p) Read(engine, data, order, p, missing, value); // Identical warmup.
            r = Measure(name, n, [&](size_t p) { return Read(engine, data, order, p, missing, value); });
        }
        FileStats(path, &r);
        results.push_back(r);
    }
    return results;
}

struct Config {
    std::vector<size_t> sizes{100000, 1000000};
    size_t value_bytes = 100, cache_bytes = 8 * 1024 * 1024, rounds = 5;
    uint64_t seed = 42;
    int bloom_bits = 0;
    size_t round_offset = 0; // 分批执行时保留轮次编号及交替的引擎顺序。
    fs::path root, output;
};
static Config Parse(int argc, char** argv) {
    Config c;
    for (int a = 1; a < argc; ++a) {
        const std::string arg(argv[a]);
        const auto equal = arg.find('=');
        const auto key = arg.substr(0, equal);
        const auto value = equal == std::string::npos ? "" : arg.substr(equal + 1);
        if (key == "--sizes") {
            c.sizes.clear(); size_t start = 0;
            do {
                const auto comma = value.find(',', start);
                c.sizes.push_back(std::stoull(value.substr(start, comma - start)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            } while (true);
        } else if (key == "--value-bytes") c.value_bytes = std::stoull(value);
        else if (key == "--cache-bytes") c.cache_bytes = std::stoull(value);
        else if (key == "--rounds") c.rounds = std::stoull(value);
        else if (key == "--seed") c.seed = std::stoull(value);
        else if (key == "--bloom-bits") c.bloom_bits = std::stoi(value);
        else if (key == "--round-offset") c.round_offset = std::stoull(value);
        else if (key == "--db-root") c.root = value;
        else if (key == "--output") c.output = value;
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (c.root.empty() || c.output.empty() || c.rounds == 0 || c.value_bytes == 0 || c.bloom_bits < 0 ||
        c.sizes.empty() || std::any_of(c.sizes.begin(), c.sizes.end(), [](size_t n) { return n == 0; }))
        throw std::runtime_error("require --db-root=<empty dir>, --output=<csv>, positive sizes/rounds/value-bytes");
    return c;
}

int main(int argc, char** argv) {
    try {
        const auto config = Parse(argc, argv);
        if (fs::exists(config.root) && !fs::is_empty(config.root))
            throw std::runtime_error("DB root must be empty (existing data is never replaced)");
        fs::create_directories(config.root);
        std::ofstream csv(config.output);
        if (!csv) throw std::runtime_error("cannot create output CSV");
        csv << "engine,round,num_keys,key_bytes,value_bytes,cache_bytes,seed,dataset_hash,request_hash,workload,ops,elapsed_ms,ops_per_sec,mib_per_sec,p50_ns,p95_ns,p99_ns,sample_stride,checksum,settle_ms,sst_files,sst_bytes,bloom_bits_per_key\n";
        csv << std::setprecision(12);
        size_t completed_rounds = 0;
        for (size_t n : config.sizes) {
            const Dataset data(n, config.value_bytes, config.seed);
            for (size_t round = 0; round < config.rounds; ++round) {
                const size_t round_number = config.round_offset + round + 1;
                // Balance which engine runs first, including across dataset sizes.
                for (int turn = 0; turn < 2; ++turn) {
                    const bool maple = ((completed_rounds + config.round_offset + round + turn) % 2 == 0);
                    const std::string engine = maple ? "MapleDB" : "LevelDB";
                    const auto root = config.root / (engine + "-" + std::to_string(n) + "-" + std::to_string(round_number));
                    if (!fs::create_directory(root)) throw std::runtime_error("run directory already exists");
                    std::cout << engine << " N=" << n << " round=" << round_number << " started" << std::endl;
                    const auto results = maple ? Run<MapleEngine>(root, data, config.cache_bytes, config.bloom_bits) :
                                                 Run<LevelEngine>(root, data, config.cache_bytes, config.bloom_bits);
                    for (const auto& r : results) {
                        const uint64_t request_hash = r.workload == "write_random" || r.workload == "get_random" ?
                            data.random_hash : r.workload == "get_hot" ? data.hot_hash : OrderHash(data.sequential);
                        const double ops_per_sec = n * 1000.0 / r.ms;
                        const double mib_per_sec = ops_per_sec * (r.workload == "get_missing" ? 16 : 16 + config.value_bytes) / (1024.0 * 1024.0);
                        csv << engine << ',' << round_number << ',' << n << ",16," << config.value_bytes << ',' << config.cache_bytes << ',' << config.seed << ','
                            << data.hash << ',' << request_hash << ',' << r.workload << ',' << n << ',' << r.ms << ',' << ops_per_sec << ',' << mib_per_sec << ','
                            << r.p50_ns << ',' << r.p95_ns << ',' << r.p99_ns << ',' << kLatencyStride << ',' << r.checksum << ',' << r.settle_ms << ',' << r.sst_files << ',' << r.sst_bytes << ',' << config.bloom_bits << '\n';
                        std::cout << "  " << r.workload << ": " << std::fixed << std::setprecision(2) << r.ms << " ms, " << ops_per_sec << " ops/s" << std::endl;
                    }
                    csv.flush();
                    if (!csv) throw std::runtime_error("CSV write failed");
                    fs::remove_all(root); // Only directories successfully created by this run.
                }
            }
            completed_rounds += config.rounds;
        }
        std::cout << "All point reads, missing reads and full scans validated. CSV: " << config.output << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "benchmark failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
