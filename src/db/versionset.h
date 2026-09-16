/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-16 10:50:24
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-16 10:52:58
 * @Description:
 * src/db/version_set.h
 * VersionSet：管理所有层级的 SSTable 文件列表
 * - 维护当前各层的 FileMetaData 列表
 * - LogAndApply：应用 VersionEdit 并持久化到 MANIFEST
 * - Recover：从 MANIFEST 恢复文件列表
 * - 提供文件查找（FindFile）等工具函数
 */

#pragma once

#include "db/dbformat.h"
#include "db/logreader.h"
#include "db/logwriter.h"
#include "db/versionedit.h"
#include "mapledb/options.h"
#include "mapledb/status.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace mapledb {

// LSM 树的层数（Level 0 ~ Level 6，共 7 层）
static constexpr int kNumLevels = 7;

// ════════════════════════════════════════════════════════
// VersionSet：所有 SSTable 文件的"目录系统"
// ════════════════════════════════════════════════════════
class VersionSet {
public:
    // dbname    : DB 目录路径（MANIFEST 等文件在此目录下）
    // options   : DB 配置
    VersionSet(const std::string& dbname, const Options& options);
    ~VersionSet();

    VersionSet(const VersionSet&)            = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    // ── 核心接口 ─────────────────────────────────────────────

    // 应用一次变更：
    //   1. 将 edit 序列化写入 MANIFEST（持久化）
    //   2. 将 edit 应用到内存中的文件列表
    //   3. 更新 next_file_number_, last_sequence_, log_number_
    Status LogAndApply(VersionEdit* edit);

    // 从 MANIFEST 恢复文件列表（DB::Open 时调用）
    // 成功后 files_[level] 包含所有 SSTable 的 FileMetaData
    Status Recover();

    // ── 文件列表访问 ──────────────────────────────────────────

    // 返回第 level 层的所有文件（按 smallest key 升序）
    const std::vector<FileMetaData*>& GetFiles(int level) const {
        assert(level >= 0 && level < kNumLevels);
        return files_[level];
    }

    // 返回第 level 层的文件数量
    int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }

    // ── 全局计数器 ────────────────────────────────────────────

    // 分配一个新的文件编号（每次调用递增）
    uint64_t NewFileNumber() { return next_file_number_++; }

    uint64_t LastSequence() const { return last_sequence_; }
    uint64_t LogNumber() const { return log_number_; }
    uint64_t ManifestFileNumber() const { return manifest_file_number_; }

    void SetLastSequence(uint64_t s) { last_sequence_ = s; }
    void SetLogNumber(uint64_t n) { log_number_ = n; }

    // ── 文件查找工具 ──────────────────────────────────────────

    // 在有序文件列表 files 中，找到第一个 largest_key >= key 的文件索引
    // （用于分层查找 SSTable）
    static int FindFile(const std::vector<FileMetaData*>& files, const Slice& key);

    // key 是否在 f 的 key 范围之后（user_key > f.largest 的 user_key）
    static bool AfterFile(const Slice& key, const FileMetaData* f);

    // key 是否在 f 的 key 范围之前（user_key < f.smallest 的 user_key）
    static bool BeforeFile(const Slice& key, const FileMetaData* f);

    // 返回 level 层中所有与 [smallest_key, largest_key] 有 key 重叠的文件
    void GetOverlappingInputs(int level, const std::string& smallest_key,
                              const std::string&          largest_key,
                              std::vector<FileMetaData*>* inputs) const;

    // 返回整个 DB 占用的磁盘空间估算值（字节）
    uint64_t TotalFileSize() const;

private:
    // ── 内部辅助 ─────────────────────────────────────────────
    //  Status ReopenManifestWriter();

    // 将 edit 中的 new_files_ 和 deleted_files_ 应用到 files_
    void ApplyEdit(const VersionEdit& edit);

    // 确保 MANIFEST 文件已打开（首次调用时创建）
    Status EnsureManifestOpen();

    // 将当前完整文件列表作为快照写入 MANIFEST（DB 首次打开时调用）
    Status WriteSnapshot();

    // ── 成员变量 ─────────────────────────────────────────────

    std::string dbname_;
    Options     options_;

    // 各层的文件列表（每层按 smallest key 排序）
    std::vector<FileMetaData*> files_[kNumLevels];

    // 全局单调递增的文件编号（每次创建新文件时 +1）
    uint64_t next_file_number_;

    // 最后一次写操作的序列号
    uint64_t last_sequence_;

    // 当前使用的 WAL 文件号
    uint64_t log_number_;

    // 当前 MANIFEST 文件号（MANIFEST 文件名 = "MANIFEST-{number}"）
    uint64_t manifest_file_number_;

    // MANIFEST 文件的 WAL 写入器（nullptr 表示还没打开）
    log::Writer* manifest_writer_;
};

} // namespace mapledb