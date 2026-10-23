// src/db/db_impl.cpp
// DBImpl 的完整实现：Open / Put / Delete / Get / Flush

#include "db/dbimpl.h"

#include "db/tablecache.h"
#include "db/writebatch.h"
#include "util/coding.h"
#include "util/fileutil.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <set>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

namespace mapledb {

namespace fs = std::filesystem;

DBImpl::DBLock::~DBLock() {
    if (fd >= 0)
        close(fd); // 最后一个句柄关闭时由内核释放 flock，异常进程退出也能释放。
}

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

// ── 构造/析构 ──────────────────────────────────────────────────

DBImpl::DBImpl(const Options& options, const std::string& dbname) :
    dbname_(dbname), options_(options), icmp_(), mem_(nullptr), imm_(nullptr), log_writer_(nullptr),
    log_number_(0), versions_(dbname, options),
    table_cache_(new TableCache(dbname, options, &icmp_, options.table_cache_entries)),
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
    if (!options_.create_if_missing && !fs::exists(dbname_ + "/CURRENT"))
        return Status::IOError("database does not exist: " + dbname_);
    Status s = CreateDBDirectory(dbname_);
    if (!s.Ok())
        return s;

    // 修复：对象内 mutex 无法保护另一个 DB/进程，读取元数据前必须取得目录排他锁。
    db_lock_.fd = open((dbname_ + "/LOCK").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (db_lock_.fd < 0 || flock(db_lock_.fd, LOCK_EX | LOCK_NB) != 0)
        return Status::IOError("database is locked or LOCK cannot be opened: " + dbname_);

    // ② 判断是新 DB 还是重启恢复
    std::string current_path = dbname_ + "/CURRENT";
    bool        is_new_db    = !fs::exists(current_path);

    // 修复：旧库丢失 CURRENT 是元数据损坏，不能重新初始化并掩盖原有数据。
    if (is_new_db) {
        for (const auto& entry : fs::directory_iterator(dbname_)) {
            const std::string name = entry.path().filename().string();
            const std::string extension = entry.path().extension().string();
            if (entry.is_regular_file() && (name.find("MANIFEST-") == 0 || extension == ".log" || extension == ".ldb"))
                return Status::Corruption("CURRENT missing from existing database: " + dbname_);
        }
    }

    if (!is_new_db) {
        // ── 重启恢复路径 ─────────────────────────────────────
        // 从 MANIFEST 恢复 SSTable 文件列表
        Status s = versions_.Recover();
        if (!s.Ok())
            return s;

        // 修复：目录中的 SSTable 缺失/大小不符时，不能把不完整数据库正常开放。
        for (int level = 0; level < kNumLevels; ++level) {
            for (const auto* file : versions_.GetFiles(level)) {
                std::error_code error;
                uint64_t size = fs::file_size(TableFileName(file->number), error);
                if (error)
                    return Status::IOError("cannot stat referenced SSTable", error.message());
                if (size != file->file_size)
                    return Status::Corruption("referenced SSTable size mismatch");
            }
        }

        // 找到所有需要重放的 WAL 文件
        // （log_number >= versions_.LogNumber() 的都需要重放）
        uint64_t              min_log = versions_.LogNumber();
        std::vector<uint64_t> logs_to_recover;

        for (const auto& entry : fs::directory_iterator(dbname_)) {
            std::string fname = entry.path().filename().string();
            // 匹配 "000XXX.log" 格式
            if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".log") {
                const std::string digits = fname.substr(0, fname.size() - 4);
                if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    continue;
                try {
                    uint64_t num = std::stoull(digits);
                    if (num == ~uint64_t{0})
                        return Status::Corruption("WAL file number overflows allocator");
                    versions_.MarkFileNumberUsed(num);
                    if (num >= min_log) {
                        logs_to_recover.push_back(num);
                    }
                } catch (const std::exception&) {
                    return Status::Corruption("invalid WAL file number");
                }
            }
        }
        std::sort(logs_to_recover.begin(), logs_to_recover.end());
        if (min_log != 0 && !std::binary_search(logs_to_recover.begin(), logs_to_recover.end(), min_log))
            return Status::Corruption("required WAL file is missing");

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
    // 修复：创建 WAL 失败时不能继续发布其文件号并返回 Open 成功。
    s = log_writer_->Sync();
    if (s.Ok())
        s = SyncDirectory(dbname_); // WAL 目录项先于 MANIFEST 中的恢复下界落盘。
    if (!s.Ok())
        return s;

    // ⑤ 更新 VersionSet 里的 log_number
    VersionEdit log_edit;
    // 修复：恢复到 MemTable 的数据尚未全部落盘，不能因创建新 WAL 就跳过旧 WAL。
    // 已有库保留恢复下界，等 Flush 成功后再推进；新库则直接从新 WAL 开始恢复。
    log_edit.SetLogNumber(is_new_db ? log_number_ : versions_.LogNumber());
    s = versions_.LogAndApply(&log_edit);
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
    Status s;
    try {
        s = impl->Open();
    } catch (const fs::filesystem_error& error) {
        // 文件系统读取失败应遵循 Status 接口，不向调用者抛出未捕获异常。
        s = Status::IOError("cannot open database", error.what());
    }
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
    Writer writer;
    writer.batch = batch;
    writers_.push_back(&writer);
    while (!writer.done && writers_.front() != &writer)
        writer.cv.wait(lock);
    if (writer.done)
        return writer.status;

    // 修改：仅队首允许轮转 mem_/WAL；其他写者等待，读者仍可取得 DB 锁。
    Status s = MakeRoomForWrite(lock);
    Writer* last = &writer;
    WriteBatch group;
    WriteBatch* updates = batch;
    SequenceNumber last_sequence = versions_.LastSequence();
    if (s.Ok()) {
        // 修改：只合并已经排队的相邻请求，不主动等待；上限限制单组占用时间。
        // sync_writes 是整个 DB 的配置，组内所有请求共享相同的持久化要求。
        constexpr size_t kMaxGroupBytes = 128 * 1024;
        size_t bytes = batch->ByteSize();
        for (size_t i = 1; i < writers_.size(); ++i) {
            Writer* next = writers_[i];
            if (bytes >= kMaxGroupBytes || next->batch->ByteSize() > kMaxGroupBytes - bytes)
                break;
            if (updates == batch) {
                group.Append(*batch);
                updates = &group;
            }
            group.Append(*next->batch);
            bytes += next->batch->ByteSize();
            last = next;
        }
        const int count = WriteBatchInternal::Count(updates);
        if (count <= 0 || static_cast<uint64_t>(count) > kMaxSequenceNumber - last_sequence) {
            s = Status::InvalidArgument("write sequence exhausted");
        } else {
            WriteBatchInternal::SetSequence(updates, last_sequence + 1);
            last_sequence += count;
            // 修改：队首身份保证单写者，解锁执行 WAL 和跳表插入，避免阻塞所有 Get。
            // 此时 LastSequence 仍为旧值，读者会跳过尚未整组提交的新版本。
            lock.unlock();
            s = log_writer_->AddRecord(WriteBatchInternal::Contents(updates), options_.sync_writes);
            if (s.Ok() && !options_.sync_writes)
                s = log_writer_->Flush();
            if (s.Ok())
                s = WriteBatchInternal::InsertInto(updates, mem_);
            lock.lock();
            if (s.Ok())
                versions_.SetLastSequence(last_sequence); // 整组完成后一次发布可见序列号。
            else if (bg_error_.Ok())
                bg_error_ = s; // 日志可能部分写入：停止后续写入，不发布失败的组。
        }
    }
    // 修改：整组返回同一结果；先出队再唤醒，避免引用已返回调用的栈节点。
    while (true) {
        Writer* ready = writers_.front();
        writers_.pop_front();
        if (ready != &writer) {
            ready->status = s;
            ready->done = true;
            ready->cv.notify_one();
        }
        if (ready == last)
            break;
    }
    if (!writers_.empty())
        writers_.front()->cv.notify_one();
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

        // 修复：新 WAL 的内容/目录项先落盘，再切换内存状态；失败则保留旧 WAL 和 mem_。
        uint64_t new_log_number = versions_.NewFileNumber();
        auto* new_log_writer = new log::Writer(LogFileName(new_log_number));
        Status s = new_log_writer->Sync();
        if (s.Ok()) s = SyncDirectory(dbname_);
        if (!s.Ok()) {
            delete new_log_writer;
            bg_error_ = s;
            return s;
        }
        // mem_ 已满且 imm_ 为空：轮转
        imm_ = mem_;
        mem_ = new MemTable(icmp_);
        mem_->Ref();
        has_imm_.store(true, std::memory_order_release); // 发布运行期新 imm，供后台优先 Flush。

        // 创建新 WAL
        delete log_writer_;
        log_number_ = new_log_number;
        log_writer_ = new_log_writer;

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
    // 修复：Flush 的过滤器按 user key 构造，与 Get 的内部键查找保持一致。
    TableBuilder builder(path, tb_opts, &icmp_);

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
    if (s.Ok())
        s = SyncDirectory(dbname_); // 新 SSTable 的目录项也必须先于 MANIFEST 持久化。
    if (!s.Ok())
        return s;

    // 修复：大小查询失败不能伪装成空文件，否则会错误地释放 imm_ 和旧 WAL。
    std::error_code size_error;
    *file_size = fs::file_size(path, size_error);
    if (size_error)
        return Status::IOError("cannot get SSTable size: " + path, size_error.message());

    return Status::OK();
}

// ════════════════════════════════════════════════════════
// 读路径：Get
// ════════════════════════════════════════════════════════

std::shared_ptr<const DBImpl::FileSnapshot> DBImpl::PinFiles() {
    if (current_files_ && current_files_->generation == versions_.FileGeneration())
        return current_files_;

    auto snapshot = std::make_shared<FileSnapshot>();
    snapshot->generation = versions_.FileGeneration();
    for (int level = 0; level < kNumLevels; ++level) {
        auto& files = snapshot->files[level];
        files.reserve(versions_.GetFiles(level).size());
        for (const auto* f : versions_.GetFiles(level))
            files.push_back(*f);
    }
    // 修改：共享后不能在 Get 中排序；L0 的新文件优先顺序在创建版本时确定。
    auto& l0 = snapshot->files[0];
    std::sort(l0.begin(), l0.end(), [](const FileMetaData& a, const FileMetaData& b) {
        return a.number > b.number;
    });
    current_files_ = snapshot;
    // 只在文件版本变化时清理过期登记，避免长期写入留下无读者的弱引用记录。
    file_snapshots_.erase(
        std::remove_if(file_snapshots_.begin(), file_snapshots_.end(),
                       [](const auto& version) { return version.expired(); }),
        file_snapshots_.end());
    file_snapshots_.push_back(snapshot);
    return current_files_;
}

Status DBImpl::Get(const std::string& key, std::string* value) {
    // ① 在锁内快照所有需要的状态
    SequenceNumber seq;
    MemTable*      mem;
    MemTable*      imm;
    std::shared_ptr<const FileSnapshot> files_snapshot;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        seq = versions_.LastSequence();
        mem = mem_;
        imm = imm_;
        mem->Ref();
        if (imm)
            imm->Ref();
        files_snapshot = PinFiles();
    }
    // 锁已释放，后台 Compaction 可以并发运行

    // ② 构造 LookupKey
    LookupKey lkey(Slice(key), seq);
    Status    s;

    // ③ 查 mem_（SkipList 支持多读者并发，无需额外锁）
    if (mem->Get(lkey, value, &s)) {
        mem->Unref();
        if (imm)
            imm->Unref();
        return s;
    }

    // ④ 查 imm_
    if (imm != nullptr && imm->Get(lkey, value, &s)) {
        mem->Unref();
        imm->Unref();
        return s;
    }

    mem->Unref();
    if (imm)
        imm->Unref();

    // ⑤ 按层查 SSTable（使用快照，无锁）
    // files_snapshot 固定旧文件，函数任何返回路径都会自动释放引用。
    Slice internal_key = lkey.internal_key();
    const Slice user_key(key);
    const Comparator* user_cmp = BytewiseComparator();

    for (int level = 0; level < kNumLevels; level++) {
        const auto& fvec = files_snapshot->files[level];
        if (fvec.empty())
            continue;

        if (level == 0) {
            // Level 0：文件可能 key 重叠，从新到旧逐个检查
            for (const FileMetaData& f : fvec) {
                // 修改：先按用户键范围排除无关 SST，避免索引/数据读取及 CRC 校验。
                // 不能比较带 tag 的原始字节，否则同键不同 sequence 可能被错误跳过。
                if (user_cmp->Compare(user_key, ExtractUserKey(Slice(f.smallest))) < 0 ||
                    user_cmp->Compare(user_key, ExtractUserKey(Slice(f.largest))) > 0)
                    continue;
                bool found = false;
                s          = SearchSST(&f, internal_key, value, &found);
                if (!s.Ok() && !s.IsNotFound())
                    return s;
                if (found)
                    return s;
            }
        } else {
            // Level 1+：文件有序无重叠，二分定位
            // 修改：直接在不可变元数据上二分，不为每次 Get 另建指针数组。
            auto candidate = std::lower_bound(
                fvec.begin(), fvec.end(), internal_key,
                [this](const FileMetaData& f, const Slice& target) {
                    return icmp_.Compare(Slice(f.largest), target) < 0;
                });
            if (candidate == fvec.end())
                continue;
            // 二分仅保证 largest >= internal_key；间隙中的 key 还可能小于 smallest。
            if (user_cmp->Compare(user_key, ExtractUserKey(Slice(candidate->smallest))) < 0)
                continue;

            bool found = false;
            s          = SearchSST(&*candidate, internal_key, value, &found);
            if (!s.Ok())
                return s;
            if (found)
                return s;
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
            return Status::Corruption("complete WAL record is too short for WriteBatch");
        }

        // 解析 WriteBatch
        WriteBatch batch;
        WriteBatchInternal::SetContents(&batch, record);
        const int count = WriteBatchInternal::Count(&batch);
        const SequenceNumber sequence = WriteBatchInternal::Sequence(&batch);
        // 修复：检查 sequence/count 后再计算末尾序列号，防止零条数下溢或越界。
        if (count < 0 || sequence > kMaxSequenceNumber ||
            (count > 0 && (sequence == 0 || static_cast<uint64_t>(count - 1) > kMaxSequenceNumber - sequence)))
            return Status::Corruption("invalid WAL sequence/count");

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
        if (count == 0)
            continue;
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

    return reader.status();
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
            if (s.Ok()) s = SyncDirectory(dbname_);
            if (s.Ok() && builder->NumEntries() > 0) {
                // 获取文件实际大小
                // 修复：查询输出文件失败时禁止注册错误的文件大小。
                std::error_code size_error;
                uint64_t fsize = fs::file_size(TableFileName(output_number), size_error);
                if (size_error) {
                    delete builder;
                    builder = nullptr;
                    return Status::IOError("cannot get compaction output size", size_error.message());
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
            // 修改：长合并期间优先处理新 imm，避免前台填满新 mem 后一直等整次 Major。
            // 仅调用 Flush 辅助函数，不递归 Major，保证当前输入文件及版本仍由本线程管理。
            if (has_imm_.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(mutex_);
                if (imm_ != nullptr) {
                    status = FlushImmutableMemTable(lock);
                    if (!status.Ok())
                        break;
                }
            }
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
                // 合并输出同样使用内部键比较器，重写的 SSTable 才会正确携带过滤器。
                builder         = new TableBuilder(TableFileName(output_number), options_, &icmp_);
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
    // 修改：撤下已过期的缓存版本，只有实际读者才能继续固定它的旧 SST。
    if (current_files_ && current_files_->generation != versions_.FileGeneration())
        current_files_.reset();
    for (auto it = file_snapshots_.begin(); it != file_snapshots_.end();) {
        if (auto snapshot = it->lock()) {
            for (const auto& level : snapshot->files)
                for (const auto& file : level)
                    live.insert(file.number);
            ++it;
        } else {
            it = file_snapshots_.erase(it);
        }
    }
    for (int level = 0; level < kNumLevels; level++) {
        for (const auto* f : versions_.GetFiles(level)) {
            live.insert(f->number);
        }
    }
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

Status DBImpl::FlushImmutableMemTable(std::unique_lock<std::mutex>& lock) {
    assert(lock.owns_lock() && imm_ != nullptr);
    if (!bg_error_.Ok())
        return bg_error_;
    MemTable* mem = imm_;
    mem->Ref();
    const uint64_t number = versions_.NewFileNumber();
    VersionEdit edit;
    std::string smallest, largest;
    uint64_t file_size = 0;
    lock.unlock();
    Status s = WriteLevel0Table(mem, &edit, number, &smallest, &largest, &file_size);
    mem->Unref();
    lock.lock();
    if (s.Ok()) {
        if (file_size > 0 && !smallest.empty()) {
            edit.AddFile(0, number, file_size, smallest, largest);
            edit.SetLogNumber(log_number_);
            s = versions_.LogAndApply(&edit);
        } else {
            fs::remove(TableFileName(number));
        }
    }
    if (s.Ok()) {
        imm_->Unref();
        imm_ = nullptr;
        has_imm_.store(false, std::memory_order_release);
        // 修改：安装成功后立即通知；不能让写者继续等后面的 Major Compaction。
        bg_cv_.notify_all();
        for (const auto& entry : fs::directory_iterator(dbname_)) {
            const std::string name = entry.path().filename().string();
            if (name.size() > 4 && name.substr(name.size() - 4) == ".log") {
                try {
                    if (std::stoull(name) < log_number_)
                        fs::remove(entry.path());
                } catch (...) {}
            }
        }
    } else {
        // 保留失败的 imm 与旧 WAL，沿用原错误锁存，后续写入不得越过持久化失败。
        bg_error_ = s;
    }
    return s;
}

void DBImpl::BackgroundCompaction() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (imm_ != nullptr && !FlushImmutableMemTable(lock).Ok())
        return;
    if (!bg_error_.Ok())
        return;
    Compaction* c = PickCompaction();
    if (c == nullptr)
        return;
    lock.unlock();
    Status s = DoCompactionWork(c);
    lock.lock();
    if (s.Ok())
        s = InstallCompactionResults(c);
    if (s.Ok())
        RemoveObsoleteFiles();
    if (!s.Ok())
        bg_error_ = s;
    if (bg_error_.Ok()) {
        int level = 0;
        if (imm_ != nullptr || NeedsCompaction(&level))
            bg_scheduled_ = true;
    }
    lock.unlock();
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
    // ① 在锁内快照所有需要的状态（与 Get 模式相同）
    SequenceNumber                         seq;
    MemTable*                              mem;
    MemTable*                              imm;
    std::shared_ptr<const FileSnapshot> files_snapshot;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        seq = versions_.LastSequence();
        mem = mem_;
        imm = imm_;
        mem->Ref();
        if (imm)
            imm->Ref();
        files_snapshot = PinFiles();
    }
    // 锁已释放

    // ② 构建各数据源的 Iterator
    std::vector<Iterator*> list;

    // MemTable（可写 MemTable）
    list.push_back(new MemTableIterator(mem));
    mem->Unref(); // 释放在此函数中持有的临时引用
                  // MemTableIterator 已在构造时 Ref，不会被提前释放

    // imm_（不可变 MemTable，等待 Flush）
    if (imm != nullptr) {
        list.push_back(new MemTableIterator(imm));
        imm->Unref();
    }

    // 各层 SSTable（通过 TableCache 获取，无需重复打开文件）
    for (int level = 0; level < kNumLevels; level++) {
        for (const FileMetaData& f : files_snapshot->files[level]) {
            Table::Iterator* it = table_cache_->NewIterator(&f);
            // 修复：失败的数据源也参与 status 汇总，不能当作空文件跳过。
            list.push_back(it);
        }
    }

    // ③ 多路归并
    Iterator* merge = new MergingIterator(std::move(list), &icmp_);
    // 子迭代器全部销毁后再解除文件引用，覆盖构建迭代器和后续扫描阶段。
    merge->RegisterCleanup([files_snapshot]() mutable { files_snapshot.reset(); });

    // ④ 用户级迭代器（tombstone 过滤 + 版本去重 + 快照隔离）
    return new DBIter(merge, seq, icmp_);
}

} // namespace mapledb
