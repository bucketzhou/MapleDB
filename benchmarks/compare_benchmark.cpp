// One request generator and timed loop for both engines. No db_bench workloads are reused.
#include "benchmark_common.h"

struct Result {
    std::string workload;
    double ms = 0, settle_ms = 0;
    double p50_ns = 0, p95_ns = 0, p99_ns = 0;
    uint64_t checksum = 0;
    size_t sst_files = 0;
    uint64_t sst_bytes = 0;
};
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
