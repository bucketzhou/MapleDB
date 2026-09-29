#include "benchmark_common.h"
#include "parallel_runner.h"

#include <array>
#include <set>

struct Request {
    enum Kind { Read, Missing, Write, Range } kind;
    size_t id;
};
struct Plan {
    std::vector<Request> requests;
    std::vector<bool> updated;
    size_t reads = 0, writes = 0, items = 0, range_length = 0;
    uint64_t hash = 14695981039346656037ULL;
};
static Plan MakePlan(const std::string& name, const Dataset& data, size_t range_length, uint64_t seed) {
    Plan plan;
    const size_t n = data.keys.size();
    plan.updated.resize(n, false);
    const bool range = name == "range";
    plan.range_length = range ? std::min(n, range_length) : 0;
    const size_t ops = range ? std::max<size_t>(1, n / plan.range_length) : n;
    plan.requests.reserve(ops);
    for (size_t p = 0; p < ops; ++p) {
        Request request{Request::Read, p};
        if (name == "write_sequential" || name == "write_random") {
            request = {Request::Write, name == "write_random" ? data.random[p] : p};
        } else if (name == "get_random") request.id = data.random[p];
        else if (name == "get_hot") request.id = data.hot[p];
        else if (name == "get_missing") request = {Request::Missing, data.random[p]};
        else if (range) request = {Request::Range, Mix(seed ^ p) % (n - plan.range_length + 1)};
        else if (name == "mixed_90r10w" || name == "mixed_50r50w") {
            const size_t write_slots = name == "mixed_90r10w" ? 1 : 5;
            if (p % 10 < write_slots) {
                request = {Request::Write, data.random[p]};
                plan.updated[request.id] = true;
            } else {
                // 读覆盖整个键空间，可与其他线程的更新重叠；每个键最多更新一次。
                request.id = Mix(seed ^ p ^ 0x72656164ULL) % n;
            }
        }
        plan.hash = Hash(plan.hash, std::string(1, char(request.kind)));
        plan.hash = Hash(plan.hash, data.keys[request.id]);
        if (request.kind == Request::Range)
            plan.hash = Hash(plan.hash, std::to_string(plan.range_length));
        if (request.kind == Request::Write) ++plan.writes;
        else ++plan.reads;
        plan.items += request.kind == Request::Range ? plan.range_length : 1;
        plan.requests.push_back(request);
    }
    return plan;
}
struct WorkerResult {
    uint64_t checksum = 0;
    size_t completed = 0;
    std::vector<double> samples;
};
struct ConcurrentResult {
    std::string name;
    Plan plan;
    double ms = 0, settle_ms = 0, p50 = 0, p95 = 0, p99 = 0;
    uint64_t checksum = 0, final_hash = 0, sst_bytes = 0;
    size_t samples = 0, sst_files = 0;
};

template<class Engine>
static void Execute(Engine& engine, const Dataset& data, const std::vector<std::string>& updates,
                    const Plan& plan, size_t threads, bool mixed, bool timed, ConcurrentResult* out) {
    std::vector<WorkerResult> results(threads);
    const size_t ops = plan.requests.size();
    const double elapsed = RunThreads(threads, [&](size_t t) {
        // 总操作数固定，连续区间划分；余数也必须执行，不能随线程数丢请求。
        const size_t begin = ops * t / threads, end = ops * (t + 1) / threads;
        // 热循环只更新线程栈上的统计，避免相邻结果槽的缓存行共享影响扩展性。
        WorkerResult result;
        result.samples.reserve((end - begin + kLatencyStride - 1) / kLatencyStride);
        std::string value;
        value.reserve(data.values.front().size());
        for (size_t p = begin; p < end; ++p) {
            const auto& request = plan.requests[p];
            const bool sample = timed && (p - begin) % kLatencyStride == 0;
            Clock::time_point start;
            if (sample) start = Clock::now();
            if (request.kind == Request::Write) {
                engine.Put(data.keys[request.id], mixed ? updates[request.id] : data.values[request.id]);
            } else if (request.kind == Request::Range) {
                // 每个请求独立创建迭代器；Seek 和所有 Next、校验都纳入范围请求耗时。
                auto cursor = engine.NewCursor();
                cursor.Seek(data.keys[request.id]);
                for (size_t k = 0; k < plan.range_length; ++k) {
                    const auto id = request.id + k;
                    if (!cursor.Valid() || cursor.Key() != data.keys[id] || cursor.Value() != data.values[id])
                        throw std::runtime_error("concurrent range key/value/count mismatch");
                    cursor.Next();
                }
                cursor.Check();
            } else {
                const bool missing = request.kind == Request::Missing;
                const bool found = engine.Get(missing ? data.missing[request.id] : data.keys[request.id], &value);
                if (missing ? found : !found)
                    throw std::runtime_error("concurrent point read presence mismatch");
                if (!missing && value != data.values[request.id] &&
                    !(mixed && plan.updated[request.id] && value == updates[request.id]))
                    throw std::runtime_error("concurrent point read returned invalid full value");
            }
            if (sample) result.samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count());
            // 混合读返回旧/新值取决于调度。校验完整内容后记录逻辑请求，不能要求两库调度相同。
            result.checksum += Mix(request.id ^ (uint64_t(request.kind) << 60));
            ++result.completed;
        }
        results[t] = std::move(result);
    });
    if (!timed) return;
    out->ms = elapsed;
    size_t completed = 0;
    std::vector<double> samples;
    for (const auto& result : results) {
        completed += result.completed;
        out->checksum += result.checksum;
        samples.insert(samples.end(), result.samples.begin(), result.samples.end());
    }
    if (completed != ops) throw std::runtime_error("concurrent operation count mismatch");
    std::sort(samples.begin(), samples.end());
    out->samples = samples.size();
    auto percentile = [&](double p) { return samples.at(size_t(p * (samples.size() - 1))); };
    out->p50 = percentile(.50); out->p95 = percentile(.95); out->p99 = percentile(.99);
}

template<class Engine>
static uint64_t VerifyFinal(Engine& engine, const Dataset& data, const std::vector<std::string>& updates,
                            const std::vector<bool>& updated) {
    uint64_t hash = 14695981039346656037ULL;
    std::string value;
    auto cursor = engine.NewCursor();
    cursor.First();
    for (size_t id = 0; id < data.keys.size(); ++id) {
        const auto& expected = updated[id] ? updates[id] : data.values[id];
        if (!engine.Get(data.keys[id], &value) || value != expected)
            throw std::runtime_error("final point read mismatch");
        if (!cursor.Valid() || cursor.Key() != data.keys[id] || cursor.Value() != expected)
            throw std::runtime_error("final scan key/value/order/count mismatch");
        hash = Hash(Hash(hash, cursor.Key()), cursor.Value());
        cursor.Next();
    }
    if (cursor.Valid()) throw std::runtime_error("final scan returned extra keys");
    cursor.Check();
    return hash;
}

template<class Engine>
static std::vector<ConcurrentResult> Run(const fs::path& root, const Dataset& data,
                                        const std::vector<std::string>& updates, size_t threads,
                                        size_t cache_bytes, int bloom_bits, size_t range_length, uint64_t seed) {
    std::vector<ConcurrentResult> rows;
    const std::array<std::string, 9> names = {"write_sequential", "write_random", "get_sequential", "get_random",
        "get_hot", "get_missing", "range", "mixed_90r10w", "mixed_50r50w"};
    for (const auto& name : names) {
        const bool writes = name.rfind("write_", 0) == 0, mixed = name.rfind("mixed_", 0) == 0;
        const auto path = root / (writes || mixed ? name : "write_random");
        if ((writes || mixed) && !fs::create_directory(path)) throw std::runtime_error("DB path exists");
        ConcurrentResult row;
        row.name = name;
        row.plan = MakePlan(name, data, range_length, seed);
        {
            Engine engine(path, cache_bytes, bloom_bits);
            if (mixed) {
                for (size_t id : data.random) engine.Put(data.keys[id], data.values[id]);
                engine.Drain();
                // 预热只读，不能在正式计时前执行更新。
                const auto warmup = MakePlan("get_random", data, range_length, seed);
                Execute(engine, data, updates, warmup, threads, false, false, &row);
            } else if (!writes) Execute(engine, data, updates, row.plan, threads, false, false, &row);
            Execute(engine, data, updates, row.plan, threads, mixed, true, &row);
            const auto settle = Clock::now();
            engine.Drain();
            if (writes || mixed) row.settle_ms = Milliseconds(Clock::now() - settle);
            row.final_hash = VerifyFinal(engine, data, updates, row.plan.updated);
        }
        if (writes || mixed) {
            Engine reopened(path, cache_bytes, bloom_bits);
            if (VerifyFinal(reopened, data, updates, row.plan.updated) != row.final_hash)
                throw std::runtime_error("reopen changed final state");
            reopened.Drain();
        }
        for (const auto& entry : fs::directory_iterator(path))
            if (entry.path().extension() == ".ldb" || entry.path().extension() == ".sst") {
                ++row.sst_files; row.sst_bytes += entry.file_size();
            }
        std::cout << "  threads=" << threads << " " << name << ": "
                  << row.plan.requests.size() * 1000.0 / row.ms << " requests/s" << std::endl;
        rows.push_back(std::move(row));
    }
    return rows;
}

struct Config {
    std::vector<size_t> sizes{100000}, threads{1, 2, 4, 8};
    size_t rounds = 5, value_bytes = 100, cache_bytes = 8 * 1024 * 1024, range_length = 64;
    uint64_t seed = 42;
    int bloom_bits = 10;
    fs::path root, output;
};
static std::vector<size_t> List(const std::string& text) {
    std::vector<size_t> result;
    std::istringstream input(text);
    std::string token;
    while (std::getline(input, token, ',')) {
        size_t consumed = 0;
        const auto value = std::stoull(token, &consumed);
        if (consumed != token.size() || token.front() == '-' || value == 0)
            throw std::runtime_error("lists require positive integers");
        result.push_back(value);
    }
    if (result.empty() || std::set<size_t>(result.begin(), result.end()).size() != result.size())
        throw std::runtime_error("empty or duplicate list entries");
    return result;
}
static size_t Positive(const std::string& text) {
    const auto values = List(text);
    if (values.size() != 1) throw std::runtime_error("scalar argument cannot be a list");
    return values.front();
}
static Config Parse(int argc, char** argv) {
    Config c;
    for (int a = 1; a < argc; ++a) {
        const std::string arg(argv[a]);
        const auto eq = arg.find('=');
        if (eq == std::string::npos) throw std::runtime_error("arguments must use --name=value");
        const auto key = arg.substr(0, eq), value = arg.substr(eq + 1);
        if (key == "--sizes") c.sizes = List(value);
        else if (key == "--threads") c.threads = List(value);
        else if (key == "--rounds") c.rounds = Positive(value);
        else if (key == "--value-bytes") c.value_bytes = Positive(value);
        else if (key == "--range-length") c.range_length = Positive(value);
        else if (key == "--cache-bytes") c.cache_bytes = std::stoull(value);
        else if (key == "--seed") c.seed = std::stoull(value);
        else if (key == "--bloom-bits") c.bloom_bits = std::stoi(value);
        else if (key == "--db-root") c.root = value;
        else if (key == "--output") c.output = value;
        else throw std::runtime_error("unknown argument: " + key);
    }
    if (c.root.empty() || c.output.empty() || c.bloom_bits < 0 ||
        *std::max_element(c.threads.begin(), c.threads.end()) > 128)
        throw std::runtime_error("require --db-root, --output and 1..128 threads");
    return c;
}

int main(int argc, char** argv) {
    try {
        const auto c = Parse(argc, argv);
        if (fs::exists(c.output)) throw std::runtime_error("output CSV already exists");
        if (fs::exists(c.root) && !fs::is_empty(c.root)) throw std::runtime_error("DB root must be empty");
        fs::create_directories(c.root);
        std::ofstream csv(c.output);
        if (!csv) throw std::runtime_error("cannot open output CSV");
        csv << "engine,round,num_keys,threads,workload,ops,read_ops,write_ops,items,range_length,elapsed_ms,settle_ms,ops_per_sec,items_per_sec,complete_ops_per_sec,p50_ns,p95_ns,p99_ns,samples,sample_stride,dataset_hash,update_hash,request_hash,checksum,final_hash,key_bytes,value_bytes,cache_bytes,seed,bloom_bits_per_key,sst_files,sst_bytes\n";
        csv << std::setprecision(12);
        size_t group = 0;
        for (size_t n : c.sizes) {
            Dataset data(n, c.value_bytes, c.seed);
            auto updates = data.values;
            uint64_t update_hash = 14695981039346656037ULL;
            for (size_t id = 0; id < n; ++id) {
                for (auto& byte : updates[id]) byte = char(static_cast<unsigned char>(byte) ^ 0x5a);
                update_hash = Hash(Hash(update_hash, data.keys[id]), updates[id]);
            }
            for (size_t round = 1; round <= c.rounds; ++round)
                for (size_t threads : c.threads) {
                    for (size_t turn = 0; turn < 2; ++turn) {
                        const bool maple = (group + round + turn) % 2 == 0;
                        const std::string engine = maple ? "MapleDB" : "LevelDB";
                        const auto root = c.root / (engine + "-" + std::to_string(n) + "-" + std::to_string(round) + "-" + std::to_string(threads));
                        if (!fs::create_directory(root)) throw std::runtime_error("run directory exists");
                        std::cout << engine << " N=" << n << " threads=" << threads << " round=" << round << std::endl;
                        const auto rows = maple ? Run<MapleEngine>(root, data, updates, threads, c.cache_bytes, c.bloom_bits, c.range_length, c.seed) :
                            Run<LevelEngine>(root, data, updates, threads, c.cache_bytes, c.bloom_bits, c.range_length, c.seed);
                        for (const auto& row : rows) {
                            const double ops = row.plan.requests.size();
                            csv << engine << ',' << round << ',' << n << ',' << threads << ',' << row.name << ','
                                << row.plan.requests.size() << ',' << row.plan.reads << ',' << row.plan.writes << ',' << row.plan.items << ',' << row.plan.range_length << ','
                                << row.ms << ',' << row.settle_ms << ',' << ops * 1000 / row.ms << ',' << row.plan.items * 1000.0 / row.ms << ','
                                << ops * 1000 / (row.ms + row.settle_ms) << ',' << row.p50 << ',' << row.p95 << ',' << row.p99 << ','
                                << row.samples << ',' << kLatencyStride << ',' << data.hash << ',' << update_hash << ',' << row.plan.hash << ','
                                << row.checksum << ',' << row.final_hash << ",16," << c.value_bytes << ',' << c.cache_bytes << ',' << c.seed << ','
                                << c.bloom_bits << ',' << row.sst_files << ',' << row.sst_bytes << '\n';
                        }
                        csv.flush();
                        if (!csv) throw std::runtime_error("CSV write failed");
                        fs::remove_all(root);
                    }
                    ++group;
                }
        }
        std::cout << "All concurrent requests, final states and write reopen checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "concurrent benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
