// src/db/db_impl.cpp
// DBImpl 的完整实现：Open / Put / Delete / Get / Flush

#include "db/dbimpl.h"

#include "db/tablecache.h"
#include "db/writebatch.h"
#include "util/coding.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <set>
#include <vector>

namespace mapledb {

namespace fs = std::filesystem;

// ── 文件名工具 ─────────────────────────────────────────────────

std::string DBImpl::TableFileName(uint64_t number) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "/%06llu.ldb", static_cast<unsigned long long>(number));
    return dbname_ + buf;
}

std::string DBImpl::LogFileName(uint64_t number) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "/%06llu.log", static_cast<unsigned long long>(number));
    return dbname_ + buf;
}

static constexpr int kTableCacheSize = 100; // 最多缓存 100 个 Table 对象

// ── 构造/析构 ──────────────────────────────────────────────────

DBImpl::DBImpl(const Options& options, const std::string& dbname) :
    dbname_(dbname), options_(options), icmp_(), mem_(nullptr), imm_(nullptr), log_writer_(nullptr),
    log_number_(0), versions_(dbname, options),
    table_cache_(new TableCache(dbname, options, &icmp_, kTableCacheSize)),
    bg_scheduled_(false),   // Day 12 新增
    shutting_down_(false) { // Day 12 新增
                            // bg_thread_ 在 Open() 末尾启动，确保单线程恢复完成
}
DBImpl::~DBImpl() {
    // ① 通知后台线程关闭（在锁内设置标志）
    {
        std::unique_lock<std::mutex> lock(mutex_);
        shutting_down_ = true;
        bg_cv_.notify_all();
    }

    // ② 等待后台线程退出（必须在释放资源前完成）
    if (bg_thread_.joinable()) {
        bg_thread_.join();
    }

    delete log_writer_;
    if (mem_)
        mem_->Unref();
    if (imm_)
        imm_->Unref();
    delete table_cache_;
}

// ════════════════════════════════════════════════════════
// DB::Open：打开/创建数据库
// ════════════════════════════════════════════════════════

Status DBImpl::Open() {
    // ① 确保 DB 目录存在
    fs::create_directories(dbname_);

    // ② 判断是新 DB 还是重启恢复
    std::string current_path = dbname_ + "/CURRENT";
    bool        is_new_db    = !fs::exists(current_path);

    if (!is_new_db) {
        // ── 重启恢复路径 ─────────────────────────────────────
        // 从 MANIFEST 恢复 SSTable 文件列表
        Status s = versions_.Recover();
        if (!s.Ok())
            return s;

        // 找到所有需要重放的 WAL 文件
        // （log_number >= versions_.LogNumber() 的都需要重放）
        uint64_t              min_log = versions_.LogNumber();
        std::vector<uint64_t> logs_to_recover;

        for (const auto& entry : fs::directory_iterator(dbname_)) {
            std::string fname = entry.path().filename().string();
            // 匹配 "000XXX.log" 格式
            if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".log") {
                try {
                    uint64_t num = std::stoull(fname);
                    if (num >= min_log) {
                        logs_to_recover.push_back(num);
                    }
                } catch (...) {}
            }
        }
        std::sort(logs_to_recover.begin(), logs_to_recover.end());

        // 重放 WAL 文件，恢复未 Flush 的数据
        SequenceNumber max_sequence = versions_.LastSequence();
        for (uint64_t log_num : logs_to_recover) {
            Status rs = RecoverLogFile(log_num, &max_sequence);
            if (!rs.Ok())
                return rs;
        }
        versions_.SetLastSequence(max_sequence);

    } else {
        // ── 全新 DB 路径 ──────────────────────────────────────
        // 写一个初始 MANIFEST（空文件列表）
        VersionEdit init_edit;
        init_edit.SetLogNumber(0);
        init_edit.SetNextFile(2);
        init_edit.SetLastSequence(0);
        Status s = versions_.LogAndApply(&init_edit);
        if (!s.Ok())
            return s;
    }

    // ③ 如果 Recover 后 mem_ 为空（没有需要恢复的 WAL），创建空 MemTable
    if (mem_ == nullptr) {
        mem_ = new MemTable(icmp_);
        mem_->Ref();
    }

    // ④ 创建新的 WAL 文件
    log_number_          = versions_.NewFileNumber();
    std::string log_path = LogFileName(log_number_);
    log_writer_          = new log::Writer(log_path);

    // ⑤ 更新 VersionSet 里的 log_number
    VersionEdit log_edit;
    log_edit.SetLogNumber(log_number_);
    Status s = versions_.LogAndApply(&log_edit);
    if (!s.Ok())
        return s;

    // 单线程恢复完成，启动后台线程
    bg_thread_ = std::thread(&DBImpl::BGWork, this);

    // 如果恢复后有 Compaction 需求，调度后台线程
    {
        std::unique_lock<std::mutex> lock(mutex_);
        MaybeScheduleCompaction();
    }

    return Status::OK();
}

// ── DB::Open 静态工厂方法 ─────────────────────────────────────

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
    *dbptr       = nullptr;
    DBImpl* impl = new DBImpl(options, dbname);
    Status  s    = impl->Open();
    if (s.Ok()) {
        *dbptr = impl;
    } else {
        delete impl;
    }
    return s;
}

// ════════════════════════════════════════════════════════
// 写路径：Put / Delete / Write
// ════════════════════════════════════════════════════════

Status DBImpl::Put(const std::string& key, const std::string& value) {
    WriteBatch batch;
    batch.Put(Slice(key), Slice(value));
    return Write(&batch);
}

Status DBImpl::Delete(const std::string& key) {
    WriteBatch batch;
    batch.Delete(Slice(key));
    return Write(&batch);
}

Status DBImpl::Write(WriteBatch* batch) {
    std::unique_lock<std::mutex> lock(mutex_);

    // ① 等待 mem_ 有空间（可能等待后台 Flush）
    Status s = MakeRoomForWrite(lock);
    if (!s.Ok())
        return s;

    // ② 分配序列号 （在锁内，原子性由锁保证）
    SequenceNumber seq = versions_.LastSequence() + 1;
    WriteBatchInternal::SetSequence(batch, seq);
    versions_.SetLastSequence(seq + WriteBatchInternal::Count(batch) - 1);

    // ③ 写 WAL（持久化保证：先写日志）
    s = log_writer_->AddRecord(WriteBatchInternal::Contents(batch));
    if (!s.Ok())
        return s;
    s = log_writer_->Flush();
    if (!s.Ok())
        return s;

    // ④ 写 MemTable（内存操作，快速）
    s = WriteBatchInternal::InsertInto(batch, mem_);
    return s;
}

// ── MakeRoomForWrite ──────────────────────────────────────────

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
    while (true) {
        if (!bg_error_.Ok())
            return bg_error_;
        if (shutting_down_)
            return Status::IOError("DB is shutting down");

        // mem_ 未满，直接返回
        if (mem_->ApproximateMemoryUsage() < options_.writer_buffer_size) {
            return Status::OK();
        }

        // imm_ 还没被 Flush，等后台线程处理完
        if (imm_ != nullptr) {
            bg_cv_.wait(lock); // 释放 mutex_，等待 BGWork 的 notify_all
            continue;          // 被唤醒后重新检查所有条件
        }

        // mem_ 已满且 imm_ 为空：轮转
        imm_ = mem_;
        mem_ = new MemTable(icmp_);
        mem_->Ref();

        // 创建新 WAL
        delete log_writer_;
        log_number_ = versions_.NewFileNumber();
        log_writer_ = new log::Writer(LogFileName(log_number_));

        // 通知后台线程开始 Flush
        ScheduleCompaction();
        return Status::OK();
    }
}

// ════════════════════════════════════════════════════════
// Minor Compaction：MemTable → Level 0 SSTable
// ════════════════════════════════════════════════════════

Status DBImpl::CompactMemTable() {
    assert(imm_ != nullptr);

    // ① 分配一个文件号给新 SSTable
    uint64_t file_number = versions_.NewFileNumber();

    // ② 遍历 imm_，写成 SSTable 文件
    VersionEdit edit;
    std::string smallest_key, largest_key;
    uint64_t    file_size = 0;

    Status s = WriteLevel0Table(imm_, &edit, file_number, &smallest_key, &largest_key, &file_size);
    if (!s.Ok()) {
        // Flush 失败，保留 imm_ 等待重试
        return s;
    }

    if (file_size == 0 || smallest_key.empty()) {
        std::string ghost_path = TableFileName(file_number);
        if (fs::exists(ghost_path)) {
            fs::remove(ghost_path); // 清理写出的空壳文件
        }
        imm_->Unref();
        imm_ = nullptr;
        return Status::OK();
    }

    // ③ 构造 VersionEdit：标记新增文件
    edit.AddFile(0, file_number, file_size, smallest_key, largest_key);
    edit.SetLogNumber(log_number_);

    // ④ 持久化到 MANIFEST，更新内存文件列表
    s = versions_.LogAndApply(&edit);
    if (!s.Ok())
        return s;

    // ⑤ 释放 imm_，删除旧 WAL
    // uint64_t old_log = versions_.LogNumber();
    imm_->Unref();
    imm_ = nullptr;

    // 删除已经 Flush 过的旧 WAL 文件
    // （只保留当前 log_number_ 对应的 WAL）
    for (const auto& entry : fs::directory_iterator(dbname_)) {
        std::string fname = entry.path().filename().string();
        if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".log") {
            try {
                uint64_t num = std::stoull(fname);
                if (num < log_number_) {
                    fs::remove(entry.path());
                }
            } catch (...) {}
        }
    }

    // 不在此处调用 MaybeScheduleCompaction()。
    // 恢复路径在 Open() 末尾统一调度后台线程。
    // MaybeScheduleCompaction();
    return Status::OK();
}

// ── WriteLevel0Table ──────────────────────────────────────────

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* /*edit*/, uint64_t file_number,
                                std::string* smallest_key, std::string* largest_key,
                                uint64_t* file_size) {
    std::string path = TableFileName(file_number);

    Options      tb_opts = options_;
    TableBuilder builder(path, tb_opts);

    // 修复：MemTable 没有嵌套 Iterator 类
    // NewIterator() 返回的是 MemTable::Table::Iterator*
    // 即 SkipList<const char*, InternalKeyComparator>::Iterator*
    MemTable::Table::Iterator* iter = mem->NewIterator();
    iter->SeekToFirst();

    bool first = true;
    while (iter->Valid()) {
        const char* entry  = iter->key(); // const char*，memtable entry 起始
        uint32_t    ik_len = 0;
        const char* p      = GetVarint32Ptr(entry, entry + 5, &ik_len);
        if (p == nullptr) {
            iter->Next();
            continue;
        }

        Slice       internal_key(p, ik_len);
        const char* vp   = p + ik_len;
        uint32_t    vlen = 0;
        vp               = GetVarint32Ptr(vp, vp + 5, &vlen);
        if (vp == nullptr) {
            iter->Next();
            continue;
        }

        Slice value(vp, vlen);

        if (first) {
            smallest_key->assign(internal_key.Data(), internal_key.Size());
            first = false;
        }
        largest_key->assign(internal_key.Data(), internal_key.Size());

        builder.Add(internal_key, value);
        iter->Next();
    }
    delete iter;

    Status s = builder.Finish();
    if (!s.Ok())
        return s;

    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        *file_size = static_cast<uint64_t>(ftell(f));
        fclose(f);
    }

    return Status::OK();
}

// ════════════════════════════════════════════════════════
// 读路径：Get
// ════════════════════════════════════════════════════════

Status DBImpl::Get(const std::string& key, std::string* value) {
    SequenceNumber seq;
    MemTable* mem;
    MemTable* imm;
    VersionSet::FileSnapshotPtr snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = versions_.LastSequence();
        mem = mem_;
        imm = imm_;
        mem->Ref();
        if (imm) imm->Ref();
        snapshot = versions_.CurrentFiles();
    }
    LookupKey lkey(Slice(key), seq);
    Status s;
    const bool in_memory = mem->Get(lkey, value, &s) ||
                           (imm != nullptr && imm->Get(lkey, value, &s));
    mem->Unref();
    if (imm) imm->Unref();
    if (in_memory) return s;

    const Slice internal_key = lkey.internal_key();
    for (int level = 0; level < kNumLevels; ++level) {
        const auto& files = (*snapshot)[level];
        if (level == 0) {
            for (const auto& f : files) {
                if (Slice(key).Compare(ExtractUserKey(Slice(f.smallest))) < 0 ||
                    Slice(key).Compare(ExtractUserKey(Slice(f.largest))) > 0) continue;
                bool found = false;
                s = SearchSST(&f, internal_key, value, &found);
                if (found || !s.Ok()) return s;
            }
        } else {
            auto it = std::lower_bound(files.begin(), files.end(), internal_key,
                [this](const FileMetaData& f, const Slice& target) {
                    return icmp_.Compare(Slice(f.largest), target) < 0;
                });
            if (it == files.end() ||
                Slice(key).Compare(ExtractUserKey(Slice(it->smallest))) < 0) continue;
            bool found = false;
            s = SearchSST(&*it, internal_key, value, &found);
            if (found || !s.Ok()) return s;
        }
    }
    return Status::NotFound(Slice(key));
}

// ── SearchSST：在一个 SSTable 文件里查找 internal_key ─────────

// Status DBImpl::SearchSST(const FileMetaData* file, const Slice& internal_key, std::string* value,
//                          bool* found) {
//     *found = false;

//     std::string path  = TableFileName(file->number);
//     Table*      table = nullptr;
//     Status      s     = Table::Open(path, options_, &icmp_, &table);
//     if (!s.Ok())
//         return s;

//     // 提取查询的 user_key（internal_key 去掉末尾 8 字节 tag）
//     Slice query_user_key(internal_key.Data(),
//                          internal_key.Size() > 8 ? internal_key.Size() - 8 : 0);

//     struct SearchResult {
//         std::string* value;
//         bool         found;
//         bool         is_deletion;
//         Slice        query_user_key; // 用于回调里验证 user_key
//     } result{value, false, false, query_user_key};

//     s = table->InternalGet(internal_key, &result, [](void* arg, const Slice& k, const Slice& v) {
//         auto* r = static_cast<SearchResult*>(arg);

//         if (k.Size() < 8)
//             return;

//         // ← 关键：验证 user_key 是否匹配
//         Slice found_user_key(k.Data(), k.Size() - 8);
//         if (found_user_key.Compare(r->query_user_key) != 0) {
//             // Seek 找到了不同 user_key 的记录（目标 key 不存在于此文件）
//             return;
//         }

//         uint64_t tag  = DecodeFixed64(k.Data() + k.Size() - 8);
//         uint8_t  type = tag & 0xFF;

//         if (type == static_cast<uint8_t>(kTypeValue)) {
//             r->value->assign(v.Data(), v.Size());
//             r->found       = true;
//             r->is_deletion = false;
//         } else {
//             // kTypeDeletion：tombstone
//             r->found       = true;
//             r->is_deletion = true;
//         }
//     });

//     if (!s.Ok()) {
//         delete table;
//         return s;
//     }

//     if (result.found) {
//         *found = true;
//         if (result.is_deletion) {
//             s = Status::NotFound(Slice());
//         }
//     }

//     delete table;
//     return s;
// }

// ── SearchSST：在一个 SSTable 文件里查找 internal_key ─────────
// Day11: 使用 table_cache_ 获取 Table（命中缓存则无需重新打开文件）

Status DBImpl::SearchSST(const FileMetaData* file, const Slice& internal_key, std::string* value,
                         bool* found) {
    *found = false;

    // 提取查询的 user_key（internal_key 去掉末尾 8 字节 tag）
    Slice query_user_key(internal_key.Data(),
                         internal_key.Size() > 8 ? internal_key.Size() - 8 : 0);

    struct SearchResult {
        std::string* value;
        bool         found;
        bool         is_deletion;
        Slice        query_user_key;
    } result{value, false, false, query_user_key};

    // Day11: 通过 table_cache_ 查找（命中缓存则无需重开文件）
    Status s = table_cache_->Get(file, internal_key, &result,
                                 [](void* arg, const Slice& k, const Slice& v) {
                                     auto* r = static_cast<SearchResult*>(arg);

                                     if (k.Size() < 8)
                                         return;

                                     // 验证 user_key 是否匹配
                                     Slice found_user_key(k.Data(), k.Size() - 8);
                                     if (found_user_key.Compare(r->query_user_key) != 0) {
                                         return;
                                     }

                                     uint64_t tag  = DecodeFixed64(k.Data() + k.Size() - 8);
                                     uint8_t  type = tag & 0xFF;

                                     if (type == static_cast<uint8_t>(kTypeValue)) {
                                         r->value->assign(v.Data(), v.Size());
                                         r->found       = true;
                                         r->is_deletion = false;
                                     } else {
                                         r->found       = true;
                                         r->is_deletion = true;
                                     }
                                 });

    if (!s.Ok())
        return s;

    if (result.found) {
        *found = true;
        if (result.is_deletion) {
            s = Status::NotFound(Slice());
        }
    }

    return s;
}

// ════════════════════════════════════════════════════════
// WAL 恢复：RecoverLogFile
// ════════════════════════════════════════════════════════

Status DBImpl::RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence) {
    std::string log_path = LogFileName(log_number);

    // 打开 WAL 文件
    log::Reader reader(log_path);
    Slice       record;
    std::string scratch;

    while (reader.ReadRecord(&record, &scratch)) {
        // 每条 WAL record 是一个 WriteBatch 的序列化内容
        if (record.Size() < WriteBatchInternal::kHeader) {
            // record 太短，跳过（可能是损坏的尾部）
            continue;
        }

        // 解析 WriteBatch
        WriteBatch batch;
        WriteBatchInternal::SetContents(&batch, record);

        // 如果 mem_ 为空，创建一个新的（Recover 时可能需要）
        if (mem_ == nullptr) {
            mem_ = new MemTable(icmp_);
            mem_->Ref();
        }

        // 将 batch 写入 MemTable
        Status s = WriteBatchInternal::InsertInto(&batch, mem_);
        if (!s.Ok())
            return s;

        // 更新 max_sequence
        SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch)
                                  + static_cast<SequenceNumber>(WriteBatchInternal::Count(&batch))
                                  - 1;
        if (last_seq > *max_sequence) {
            *max_sequence = last_seq;
        }

        // 如果 MemTable 已超过阈值，触发 Flush
        if (mem_->ApproximateMemoryUsage() >= options_.writer_buffer_size) {
            imm_      = mem_;
            mem_      = nullptr;
            Status cs = CompactMemTable();
            if (!cs.Ok())
                return cs;
        }
    }

    return Status::OK();
}

namespace {

class MergeIterator {
public:
    // 接管所有 Table::Iterator* 的所有权
    MergeIterator(std::vector<Table::Iterator*> iters, const InternalKeyComparator* cmp) :
        iters_(std::move(iters)), cmp_(cmp), current_(-1) {}

    ~MergeIterator() {
        for (auto* it : iters_)
            delete it;
    }

    void SeekToFirst() {
        for (auto* it : iters_)
            it->SeekToFirst();
        FindSmallest(); // 找所有迭代器中最小的
    }

    bool         Valid() const { return current_ >= 0; }
    const Slice& key() const { return iters_[current_]->key(); }
    const Slice& value() const { return iters_[current_]->value(); }

    void Next() {
        assert(Valid());
        iters_[current_]->Next(); // 当前最小迭代器前进一步
        FindSmallest();           // 重新找最小
    }
    Status status() const {
        for (auto* it : iters_) {
            if (!it->status().Ok())
                return it->status();
        }
        return Status::OK();
    }

private:
    void FindSmallest() {
        current_ = -1;
        for (int i = 0; i < static_cast<int>(iters_.size()); i++) {
            if (!iters_[i]->Valid())
                continue;
            if (current_ < 0 || cmp_->Compare(iters_[i]->key(), iters_[current_]->key()) < 0) {
                current_ = i;
            }
        }
    }

    std::vector<Table::Iterator*> iters_;
    const InternalKeyComparator*  cmp_;
    int                           current_; // -1 表示无效
};

} // anonymous namespace

uint64_t DBImpl::LevelMaxBytes(int level) {
    // L1: 10MB, L2: 100MB, L3: 1GB ...（每层 ×10）
    uint64_t result = 10ULL * 1024 * 1024; // L1 基准 = 10MB
    for (int i = 1; i < level; i++) {
        result *= 10;
    }
    return result;
}

bool DBImpl::NeedsCompaction(int* level) const {
    // L0：文件数超过阈值
    if (versions_.NumFiles(0) >= options_.l0_compaction_trigger) {
        *level = 0;
        return true;
    }
    // L1+：总大小超过阈值（不检查最后一层，无下一层可合并到）
    for (int l = 1; l < kNumLevels - 1; l++) {
        uint64_t total = 0;
        for (const auto* f : versions_.GetFiles(l)) {
            total += f->file_size;
        }
        if (total > LevelMaxBytes(l)) {
            *level = l;
            return true;
        }
    }
    return false;
}

DBImpl::Compaction* DBImpl::PickCompaction() {
    int level = 0;
    if (!NeedsCompaction(&level))
        return nullptr;

    auto* c  = new Compaction();
    c->level = level;

    const auto& level_files = versions_.GetFiles(level);
    if (level_files.empty()) {
        delete c;
        return nullptr;
    }

    if (level == 0) {
        // L0：必须全选（文件间可能有 key 重叠）
        c->inputs[0] = level_files;
    } else {
        // L1+：选第一个文件（简化策略）
        c->inputs[0].push_back(level_files[0]);
    }

    // 计算 inputs[0] 的整体 key 范围
    std::string smallest, largest;
    for (const auto* f : c->inputs[0]) {
        if (smallest.empty() || icmp_.Compare(Slice(f->smallest), Slice(smallest)) < 0)
            smallest = f->smallest;
        if (largest.empty() || icmp_.Compare(Slice(f->largest), Slice(largest)) > 0)
            largest = f->largest;
    }

    // 找 level+1 中与上述范围有重叠的文件
    versions_.GetOverlappingInputs(level + 1, smallest, largest, &c->inputs[1]);

    return c;
}

Status DBImpl::DoCompactionWork(Compaction* c) {
    Status status;

    // ① 创建各输入文件的迭代器
    //    使用作用域块确保迭代器在 tables 之前析构
    std::vector<Table*> tables;
    {
        std::vector<Table::Iterator*> iters;

        for (int which = 0; which < 2; which++) {
            for (const auto* f : c->inputs[which]) {
                Table* t = nullptr;
                status   = Table::Open(TableFileName(f->number), options_, &icmp_, &t);
                if (!status.Ok()) {
                    for (auto* it : iters)
                        delete it;
                    for (auto* tbl : tables)
                        delete tbl;
                    return status;
                }
                tables.push_back(t);
                iters.push_back(t->NewIterator());
            }
        }

        // ② 多路归并迭代器（接管 iters 所有权）
        MergeIterator merge(std::move(iters), &icmp_);

        // ③ 合并状态
        std::string current_user_key;
        bool        has_current_user_key = false;

        TableBuilder* builder       = nullptr;
        uint64_t      output_number = 0;
        std::string   output_smallest;
        std::string   output_largest;

        // 完成当前输出文件的辅助 lambda
        auto FinishCurrentOutput = [&]() -> Status {
            if (builder == nullptr)
                return Status::OK();

            Status s = builder->Finish();
            if (s.Ok() && builder->NumEntries() > 0) {
                // 获取文件实际大小
                uint64_t fsize = 0;
                FILE*    fp    = fopen(TableFileName(output_number).c_str(), "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    fsize = static_cast<uint64_t>(ftell(fp));
                    fclose(fp);
                }
                auto* meta      = new FileMetaData();
                meta->number    = output_number;
                meta->file_size = fsize;
                meta->smallest  = output_smallest;
                meta->largest   = output_largest;
                c->outputs.push_back(meta);
            } else if (s.Ok()) {
                // 空文件，删掉
                fs::remove(TableFileName(output_number));
            }
            delete builder;
            builder = nullptr;
            return s;
        };

        // ④ 归并主循环
        for (merge.SeekToFirst(); merge.Valid() && status.Ok(); merge.Next()) {
            const Slice ik  = merge.key();
            const Slice val = merge.value();

            if (ik.Size() < 8)
                continue; // 跳过格式异常的 key

            Slice     user_key = ExtractUserKey(ik);
            ValueType type     = GetValueType(ik);

            bool drop = false;

            if (has_current_user_key && Slice(current_user_key).Compare(user_key) == 0) {
                // 同一 user_key 的旧版本，直接丢弃
                drop = true;
            } else {
                current_user_key.assign(user_key.Data(), user_key.Size());
                has_current_user_key = true;
            }

            // 尝试丢弃 tombstone：只有当更深层级不存在该 user_key 时才安全丢弃
            if (!drop && type == kTypeDeletion) {
                bool has_deeper = false;
                for (int l = c->level + 2; l < kNumLevels && !has_deeper; l++) {
                    for (const auto* f : versions_.GetFiles(l)) {
                        Slice fsmall = ExtractUserKey(Slice(f->smallest));
                        Slice flarge = ExtractUserKey(Slice(f->largest));
                        if (user_key.Compare(fsmall) >= 0 && user_key.Compare(flarge) <= 0) {
                            has_deeper = true;
                            break;
                        }
                    }
                }
                if (!has_deeper)
                    drop = true;
            }

            if (drop)
                continue;

            // ⑤ 写入输出文件
            if (builder == nullptr) {
                output_number   = versions_.NewFileNumber();
                output_smallest = std::string(ik.Data(), ik.Size());
                output_largest  = output_smallest;
                builder         = new TableBuilder(TableFileName(output_number), options_);
            }

            output_largest = std::string(ik.Data(), ik.Size());
            builder->Add(ik, val);

            // 超过单文件大小限制则切割输出文件
            if (builder->FileSize() >= options_.max_file_size) {
                status = FinishCurrentOutput();
            }
        }

        // ⑥ 完成最后一个输出文件
        if (status.Ok()) {
            status = FinishCurrentOutput();
        } else {
            // 出错时也要清理 builder
            if (builder) {
                builder->Abandon();
                delete builder;
                builder = nullptr;
            }
        }

        if (status.Ok())
            status = merge.status();

    } // merge（及其迭代器）在此析构，之后才安全删除 tables

    for (auto* t : tables)
        delete t;
    return status;
}

Status DBImpl::InstallCompactionResults(Compaction* c) {
    VersionEdit edit;

    // 标记删除所有输入文件
    for (const auto* f : c->inputs[0])
        edit.DeleteFile(c->level, f->number);
    for (const auto* f : c->inputs[1])
        edit.DeleteFile(c->level + 1, f->number);

    // 注册所有输出文件（在 level+1）
    for (const auto* f : c->outputs)
        edit.AddFile(c->level + 1, f->number, f->file_size, f->smallest, f->largest);

    return versions_.LogAndApply(&edit);
}

// void DBImpl::RemoveObsoleteFiles() {
//     // 收集所有"活跃"文件号（VersionSet 当前持有的所有 SSTable）
//     std::set<uint64_t> live;
//     for (int level = 0; level < kNumLevels; level++) {
//         for (const auto* f : versions_.GetFiles(level)) {
//             live.insert(f->number);
//         }
//     }
//     // 保留当前 WAL 和 MANIFEST 文件
//     live.insert(log_number_);
//     live.insert(versions_.ManifestFileNumber());

//     // 扫描目录，删除不在 live 集合里的 .ldb 文件
//     for (const auto& entry : fs::directory_iterator(dbname_)) {
//         const std::string fname = entry.path().filename().string();
//         if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".ldb") {
//             try {
//                 uint64_t num = std::stoull(fname);
//                 if (live.find(num) == live.end()) {
//                     fs::remove(entry.path());
//                 }
//             } catch (...) {}
//         }
//     }
// }

// ── RemoveObsoleteFiles ───────────────────────────────────────

void DBImpl::RemoveObsoleteFiles() {
    // 收集所有"活跃"文件号（VersionSet 当前持有的所有 SSTable）
    std::set<uint64_t> live;
    for (int level = 0; level < kNumLevels; level++) {
        for (const auto* f : versions_.GetFiles(level)) {
            live.insert(f->number);
        }
    }
    versions_.AddLiveFiles(&live);

    // 保留当前 WAL 和 MANIFEST 文件
    live.insert(log_number_);
    live.insert(versions_.ManifestFileNumber());

    // 扫描目录，删除不在 live 集合里的 .ldb 文件
    for (const auto& entry : fs::directory_iterator(dbname_)) {
        const std::string fname = entry.path().filename().string();
        if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".ldb") {
            try {
                uint64_t num = std::stoull(fname);
                if (live.find(num) == live.end()) {
                    // Day11: 删除文件前先从 TableCache 中淘汰，避免悬空指针
                    table_cache_->Evict(num);
                    fs::remove(entry.path());
                }
            } catch (...) {}
        }
    }
}

// ── MaybeScheduleCompaction ───────────────────────────────────

// void DBImpl::MaybeScheduleCompaction() {
//     int level = 0;
//     if (!NeedsCompaction(&level))
//         return;

//     Compaction* c = PickCompaction();
//     if (c == nullptr)
//         return;

//     Status s = DoCompactionWork(c);
//     if (s.Ok()) {
//         s = InstallCompactionResults(c);
//     }
//     if (s.Ok()) {
//         RemoveObsoleteFiles();
//     }

//     delete c;

//     // 递归检查：一次 Compaction 完成后可能触发下一层
//     if (s.Ok()) {
//         MaybeScheduleCompaction();
//     }
// }

void DBImpl::BGWork() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        bg_cv_.wait(lock, [this]() { return bg_scheduled_ || shutting_down_; });

        if (shutting_down_)
            break;

        bg_scheduled_ = false;
        lock.unlock();
        BackgroundCompaction(); // 无锁执行（内部按需获取释放锁）
        lock.lock();
        bg_cv_.notify_all(); // 唤醒所有等待 imm_ 清空的写入者
    }
}

void DBImpl::ScheduleCompaction() {
    if (!bg_scheduled_) {
        bg_scheduled_ = true;
        bg_cv_.notify_one(); // 唤醒后台线程（只有一个后台线程）
    }
}

void DBImpl::MaybeScheduleCompaction() {
    // 如果已调度、正在关闭、或有错误，跳过
    if (bg_scheduled_ || shutting_down_ || !bg_error_.Ok())
        return;
    int level = 0;
    if (imm_ != nullptr || NeedsCompaction(&level)) {
        ScheduleCompaction();
    }
}

void DBImpl::BackgroundCompaction() {
    // ════════════════════════════════════════════════════
    // Phase 1: Minor Compaction（imm_ → Level 0 SSTable）
    // ════════════════════════════════════════════════════
    mutex_.lock();

    if (imm_ != nullptr) {
        // 在锁内预分配文件号，避免与写入者的 NewFileNumber 并发
        MemTable* imm_to_flush = imm_;
        imm_to_flush->Ref(); // 持有引用，防止被提前释放
        uint64_t file_number = versions_.NewFileNumber();
        mutex_.unlock();

        // ── I/O 阶段（无锁，写入者可并发写 mem_）──────────────
        VersionEdit edit;
        std::string smallest_key, largest_key;
        uint64_t    file_size = 0;
        Status s = WriteLevel0Table(imm_to_flush, &edit, file_number, &smallest_key, &largest_key,
                                    &file_size);
        imm_to_flush->Unref(); // 释放我们持有的引用

        // ── 重新获锁，安装结果 ─────────────────────────────────
        mutex_.lock();
        if (s.Ok()) {
            if (file_size > 0 && !smallest_key.empty()) {
                edit.AddFile(0, file_number, file_size, smallest_key, largest_key);
                edit.SetLogNumber(log_number_);
                s = versions_.LogAndApply(&edit);
            } else {
                // imm_ 为空，清理 ghost 文件
                std::string ghost = TableFileName(file_number);
                if (fs::exists(ghost))
                    fs::remove(ghost);
            }
        }

        if (s.Ok()) {
            imm_->Unref();
            imm_ = nullptr;
            // 清理已 Flush 的旧 WAL 文件（在锁内快速完成）
            for (const auto& entry : fs::directory_iterator(dbname_)) {
                std::string fname = entry.path().filename().string();
                if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".log") {
                    try {
                        uint64_t num = std::stoull(fname);
                        if (num < log_number_) {
                            fs::remove(entry.path());
                        }
                    } catch (...) {}
                }
            }
        } else {
            bg_error_ = s;
        }
        // notify_all 由 BGWork 在函数返回后统一发送
    }

    mutex_.unlock();

    if (!bg_error_.Ok())
        return;

    // ════════════════════════════════════════════════════
    // Phase 2: Major Compaction
    // ════════════════════════════════════════════════════

    // 在锁内选择需要 Compaction 的文件
    mutex_.lock();
    Compaction* c = PickCompaction();
    mutex_.unlock();

    if (c == nullptr)
        return;

    // ── I/O 阶段（无锁）────────────────────────────────────────
    // 多路归并：读输入 SSTable、去重、写输出 SSTable
    // 注意：DoCompactionWork 内部读 versions_.GetFiles() 是安全的，
    // 因为只有本后台线程（单线程）调用 InstallCompactionResults 修改 files_。
    Status s = DoCompactionWork(c);

    // ── 重新获锁，安装结果并清理 ───────────────────────────────
    mutex_.lock();
    if (s.Ok()) {
        s = InstallCompactionResults(c);
    }
    if (s.Ok()) {
        RemoveObsoleteFiles();
    }
    if (!s.Ok()) {
        bg_error_ = s;
    }
    // 如果还有更多 Compaction 工作，继续调度
    if (bg_error_.Ok()) {
        int level = 0;
        if (imm_ != nullptr || NeedsCompaction(&level)) {
            bg_scheduled_ = true; // BGWork 循环会立即执行下一轮
        }
    }
    mutex_.unlock();

    delete c;
}

// ════════════════════════════════════════════════════════
// NewIterator — 全局有序迭代器
// ════════════════════════════════════════════════════════

// NewIterator：创建合并了 MemTable + imm_ + 所有 SSTable 的全局迭代器。
// 实现步骤：
//   1. 在锁内快照 sequence、MemTable 指针、FileMetaData（与 Get 相同模式）
//   2. 为每个数据源创建内部 Iterator
//   3. 用 MergingIterator 归并所有内部 Iterator
//   4. 用 DBIter 包装，实现 tombstone 过滤 + 版本去重 + 快照隔离

Iterator* DBImpl::NewIterator() {
    SequenceNumber seq;
    MemTable* mem;
    MemTable* imm;
    VersionSet::FileSnapshotPtr snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = versions_.LastSequence();
        mem = mem_;
        imm = imm_;
        mem->Ref();
        if (imm) imm->Ref();
        snapshot = versions_.CurrentFiles();
    }
    std::vector<Iterator*> inputs;
    inputs.push_back(new MemTableIterator(mem));
    mem->Unref();
    if (imm) { inputs.push_back(new MemTableIterator(imm)); imm->Unref(); }
    for (int level = 0; level < kNumLevels; ++level)
        for (const auto& file : (*snapshot)[level])
            inputs.push_back(table_cache_->NewIterator(&file));
    auto* result = new DBIter(new MergingIterator(std::move(inputs), &icmp_), seq, icmp_);
    result->RegisterCleanup([](void* p, void*) {
        // 子迭代器销毁后再释放文件快照，避免 GC 提前删除旧 SST。
        delete static_cast<VersionSet::FileSnapshotPtr*>(p);
    }, new VersionSet::FileSnapshotPtr(std::move(snapshot)), nullptr);
    return result;
}

} // namespace mapledb
