/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-16 10:53:53
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 15:19:27
 * @Description:
 * src/db/version_set.cpp
 * 修复要点：
 * 1. LogAndApply 里不覆盖 edit 的 next_file 和 last_sequence（改为只补全未设置的字段）
 * 2. Recover 后正确设置 manifest_file_number_ 和 manifest_writer_
 * 3. ApplyEdit 里 next_file_number_ 更新逻辑改为"直接取更大值"，不做 +1 的歧义处理
 */

#if 1

    #include "db/versionset.h"

    #include "util/coding.h"
    #include "util/fileutil.h"

    #include <algorithm>
    #include <cassert>
    #include <cstdio>
    #include <filesystem>
    #include <sstream>
    #include <system_error>
    #include <unistd.h>

namespace mapledb {

namespace fs = std::filesystem;

// ── 文件名工具函数 ────────────────────────────────────────────

static std::string ManifestFileName(const std::string& dbname, uint64_t number) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
    return dbname + buf;
}

static std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

static Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number) {
    char numstr[32];
    snprintf(numstr, sizeof(numstr), "%06llu", static_cast<unsigned long long>(manifest_number));
    std::string manifest_base = std::string("MANIFEST-") + numstr;

    std::string current_file = CurrentFileName(dbname);
    std::string temporary = current_file + ".tmp";
    FILE*       f            = fopen(temporary.c_str(), "w");
    if (!f) {
        return Status::IOError("cannot write CURRENT: " + current_file);
    }
    // 修复：CURRENT 的缓冲写入与关闭都可能失败，错误必须阻止后续安装。
    Status s;
    if (fprintf(f, "%s\n", manifest_base.c_str()) < 0)
        s = Status::IOError("cannot write CURRENT: " + current_file);
    // 修复：先同步临时文件，再原子替换 CURRENT，避免截断原有有效指针。
    if (s.Ok() && (fflush(f) != 0 || fsync(fileno(f)) != 0))
        s = Status::IOError("cannot sync CURRENT: " + current_file);
    if (fclose(f) != 0 && s.Ok())
        s = Status::IOError("cannot close CURRENT: " + current_file);
    if (s.Ok() && rename(temporary.c_str(), current_file.c_str()) != 0)
        s = Status::IOError("cannot replace CURRENT: " + current_file);
    if (s.Ok())
        s = SyncDirectory(dbname);
    return s;
}

// ── VersionSet 构造/析构 ──────────────────────────────────────

VersionSet::VersionSet(const std::string& dbname, const Options& options) :
    dbname_(dbname), options_(options), next_file_number_(2), last_sequence_(0), log_number_(0),
    manifest_file_number_(0), manifest_writer_(nullptr) {
}

VersionSet::~VersionSet() {
    delete manifest_writer_;
    for (int level = 0; level < kNumLevels; level++) {
        for (FileMetaData* f : files_[level]) {
            delete f;
        }
        files_[level].clear();
    }
}

// ── ApplyEdit ─────────────────────────────────────────────────

void VersionSet::ApplyEdit(const VersionEdit& edit) {
    // 修复1：next_file_number_ 取"当前值"和"edit值+1"的最大值
    // 不做 "edit.value >= current" 的条件判断，直接更新为更大的
    // Day 12（原子 CAS 更新为更大值）：
    if (edit.has_next_file_number()) {
        uint64_t nfn = edit.next_file_number();
        uint64_t cur = next_file_number_.load(std::memory_order_relaxed);
        while (nfn > cur) {
            if (next_file_number_.compare_exchange_weak(cur, nfn, std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
                break;
            }
            // CAS 失败：cur 已被更新，重新比较
        }
    }
    if (edit.has_last_sequence() && edit.last_sequence() >= last_sequence_) {
        last_sequence_ = edit.last_sequence();
    }
    if (edit.has_log_number()) {
        log_number_ = edit.log_number();
    }

    // 删除文件
    for (const auto& [level, file_num] : edit.deleted_files()) {
        auto& fv = files_[level];
        for (auto it = fv.begin(); it != fv.end(); ++it) {
            if ((*it)->number == file_num) {
                delete *it;
                fv.erase(it);
                break;
            }
        }
    }

    // 新增文件
    for (const auto& [level, f] : edit.new_files()) {
        FileMetaData* meta = new FileMetaData(f);
        files_[level].push_back(meta);
    }

    // 修复：编码后的 tag 不按原始字节序排序；必须与 FindFile 的内部键比较一致。
    InternalKeyComparator icmp;
    for (int level = 0; level < kNumLevels; level++) {
        std::sort(
            files_[level].begin(), files_[level].end(),
            [&icmp](const FileMetaData* a, const FileMetaData* b) {
                return icmp.Compare(Slice(a->smallest), Slice(b->smallest)) < 0;
            });
    }
}

// ── EnsureManifestOpen ───────────────────────────────────────

Status VersionSet::EnsureManifestOpen() {
    if (manifest_writer_ != nullptr)
        return Status::OK();

    Status s = CreateDBDirectory(dbname_);
    if (!s.Ok())
        return s;
    // 分配新文件号给 MANIFEST
    manifest_file_number_     = next_file_number_.fetch_add(1, std::memory_order_relaxed);
    std::string manifest_path = ManifestFileName(dbname_, manifest_file_number_);

    // 创建新 MANIFEST（wb，覆盖写）
    manifest_writer_ = new log::Writer(manifest_path);

    // 修复：完整快照及其目录项可靠落盘后，才能让 CURRENT 指向新 MANIFEST。
    s = WriteSnapshot();
    if (s.Ok()) s = manifest_writer_->Sync();
    if (s.Ok()) s = SyncDirectory(dbname_);
    if (s.Ok()) s = SetCurrentFile(dbname_, manifest_file_number_);
    if (!s.Ok()) {
        delete manifest_writer_;
        manifest_writer_ = nullptr;
    }
    return s;
}

// ── WriteSnapshot ────────────────────────────────────────────

Status VersionSet::WriteSnapshot() {
    VersionEdit snapshot;
    snapshot.SetComparatorName(Slice("leveldb.BytewiseComparator"));
    snapshot.SetLogNumber(log_number_);
    snapshot.SetNextFile(next_file_number_.load(std::memory_order_relaxed));
    snapshot.SetLastSequence(last_sequence_);

    for (int level = 0; level < kNumLevels; level++) {
        for (const FileMetaData* f : files_[level]) {
            snapshot.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
        }
    }

    std::string record;
    snapshot.EncodeTo(&record);
    return manifest_writer_->AddRecord(Slice(record));
}

// ── LogAndApply ───────────────────────────────────────────────

Status VersionSet::LogAndApply(VersionEdit* edit) {
    // 修复3：只补全"用户没有设置的字段"，不覆盖用户已设置的值
    // （原代码直接 edit->SetNextFile(next_file_number_) 会覆盖用户传入的值）
    if (!edit->has_next_file_number()) {
        edit->SetNextFile(next_file_number_.load(std::memory_order_relaxed));
    }
    if (!edit->has_last_sequence()) {
        edit->SetLastSequence(last_sequence_);
    }
    if (!edit->has_log_number()) {
        edit->SetLogNumber(log_number_);
    }

    // 确保 MANIFEST 已打开
    Status s = EnsureManifestOpen();
    if (!s.Ok())
        return s;

    // 序列化并写入 MANIFEST
    std::string record;
    edit->EncodeTo(&record);
    s = manifest_writer_->AddRecord(Slice(record));
    if (!s.Ok())
        return s;

    s = manifest_writer_->Sync();
    if (!s.Ok())
        return s;

    // 应用到内存
    ApplyEdit(*edit);

    return Status::OK();
}

// ── Recover ──────────────────────────────────────────────────

Status VersionSet::Recover() {
    // ① 读 CURRENT
    std::string current_path = CurrentFileName(dbname_);
    FILE*       cf           = fopen(current_path.c_str(), "r");
    if (!cf) {
        return Status::IOError("CURRENT file not found: " + current_path);
    }
    char current_content[256] = {};
    if (!fgets(current_content, sizeof(current_content), cf)) {
        fclose(cf);
        return Status::Corruption("CURRENT file is empty");
    }
    fclose(cf);

    std::string manifest_name(current_content);
    while (!manifest_name.empty()
           && (manifest_name.back() == '\n' || manifest_name.back() == '\r')) {
        manifest_name.pop_back();
    }
    if (manifest_name.empty()) {
        return Status::Corruption("CURRENT file has no content");
    }

    // ② 解析 MANIFEST 文件号
    const std::string prefix = "MANIFEST-";
    if (manifest_name.size() > prefix.size() && manifest_name.substr(0, prefix.size()) == prefix) {
        try {
            manifest_file_number_ = std::stoull(manifest_name.substr(prefix.size()));
        } catch (...) {
            return Status::Corruption("bad MANIFEST file number in CURRENT");
        }
    } else {
        return Status::Corruption("bad CURRENT file content: " + manifest_name);
    }

    // 修复4：next_file_number_ 初始值设为 manifest_file_number_+1
    // 后续 ApplyEdit 会根据 edit 里的值继续更新
    next_file_number_.store(manifest_file_number_ + 1, std::memory_order_relaxed);

    std::string manifest_path = dbname_ + "/" + manifest_name;

    // ③ 逐条读取 MANIFEST 并重放
    log::Reader reader(manifest_path);
    Slice       record;
    std::string scratch;

    int record_count = 0; // ← 加这行

    while (reader.ReadRecord(&record, &scratch)) {
        record_count++;
        printf("[Recover] Read record #%d, size=%zu\n", record_count, record.Size()); // ← 加这行

        VersionEdit edit;
        Status      s = edit.DecodeFrom(record);
        if (!s.Ok()) {
            printf("[Recover] DecodeFrom FAILED: %s\n", s.ToString().c_str()); // ← 加这行
            return Status::Corruption("MANIFEST record decode error: " + s.ToString());
        }
        printf("[Recover] Edit: nfiles=%zu, dfiles=%zu\n", // ← 加这行
               edit.new_files().size(), edit.deleted_files().size());

        ApplyEdit(edit);

        printf("[Recover] After Apply: L0=%d L1=%d\n", // ← 加这行
               NumFiles(0), NumFiles(1));
    }

    // 修复：校验/读取失败不能当作普通尾部截断，否则会破坏原始故障证据。
    if (!reader.status().Ok())
        return reader.status();
    if (record_count == 0)
        return Status::Corruption("MANIFEST has no complete records");
    printf("[Recover] Total records read: %d\n", record_count); // ← 加这行

    // ④ 确保 next_file_number_ 不小于所有已知文件号+1
    for (int level = 0; level < kNumLevels; level++) {
        for (const FileMetaData* f : files_[level]) {
            uint64_t cur = next_file_number_.load(std::memory_order_relaxed);
            if (f->number + 1 > cur) {
                next_file_number_.store(f->number + 1, std::memory_order_relaxed);
            }
        }
    }
    // manifest 本身也要算进去
    {
        uint64_t cur = next_file_number_.load(std::memory_order_relaxed);
        if (manifest_file_number_ + 1 > cur) {
            next_file_number_.store(manifest_file_number_ + 1, std::memory_order_relaxed);
        }
    }

    // ⑤ 修复5：Recover 之后，manifest_writer_ 必须正确指向现有 MANIFEST
    //    使用文件实际大小初始化 block_offset_，保证追加写正确
    delete manifest_writer_;
    manifest_writer_ = nullptr;

    std::error_code size_error;
    const uint64_t  manifest_size = fs::file_size(manifest_path, size_error);
    if (size_error) {
        return Status::IOError("cannot get MANIFEST size: " + manifest_path, size_error.message());
    }
    // 修复：不能在中断写入留下的残尾后追加，否则新记录下次恢复时也会被丢弃。
    const uint64_t valid_size = reader.LastRecordEndOffset();
    if (manifest_size != valid_size) {
        fs::resize_file(manifest_path, valid_size, size_error);
        if (size_error)
            return Status::IOError("cannot truncate MANIFEST tail", size_error.message());
    }
    manifest_writer_ = new log::Writer(manifest_path, valid_size);

    return manifest_writer_->Sync(); // 将恢复时的截断也可靠落盘。
}

// ── FindFile ─────────────────────────────────────────────────

int VersionSet::FindFile(const std::vector<FileMetaData*>& files, const Slice& key) {
    InternalKeyComparator icmp;
    int                   left  = 0;
    int                   right = static_cast<int>(files.size());
    while (left < right) {
        int mid = (left + right) / 2;
        // 用 InternalKeyComparator 正确比较（user_key 升序，seq 降序）
        if (icmp.Compare(Slice(files[mid]->largest), key) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

bool VersionSet::AfterFile(const Slice& key, const FileMetaData* f) {
    return !f->largest.empty() && key.ToString() > f->largest;
}

bool VersionSet::BeforeFile(const Slice& key, const FileMetaData* f) {
    return !f->smallest.empty() && key.ToString() < f->smallest;
}

void VersionSet::GetOverlappingInputs(int level, const std::string& smallest_key,
                                      const std::string&          largest_key,
                                      std::vector<FileMetaData*>* inputs) const {
    inputs->clear();
    // for (FileMetaData* f : files_[level]) {
    //     if (f->largest >= smallest_key && f->smallest <= largest_key) {
    //         inputs->push_back(f);
    //     }
    // }

    // 去掉 8 字节 tag，只留 user_key
    auto ExtractUK = [](const std::string& ik) -> std::string {
        return ik.size() >= 8 ? ik.substr(0, ik.size() - 8) : ik;
    };
    std::string small_uk = ExtractUK(smallest_key);
    std::string large_uk = ExtractUK(largest_key);

    for (FileMetaData* f : files_[level]) {
        std::string f_small_uk = ExtractUK(f->smallest);
        std::string f_large_uk = ExtractUK(f->largest);
        // 区间重叠：[f_small, f_large] ∩ [small, large] ≠ ∅
        // ⟺ f_large >= small AND f_small <= large
        if (f_large_uk >= small_uk && f_small_uk <= large_uk) {
            inputs->push_back(f);
        }
    }
}

uint64_t VersionSet::TotalFileSize() const {
    uint64_t total = 0;
    for (int level = 0; level < kNumLevels; level++) {
        for (const FileMetaData* f : files_[level]) {
            total += f->file_size;
        }
    }
    return total;
}

} // namespace mapledb

#endif
