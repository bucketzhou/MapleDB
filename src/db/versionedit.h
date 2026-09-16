/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-16 10:39:43
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-16 10:50:03
 * @Description: FileMetaData：SSTable 文件元信息 VersionEdit ：一次文件集合的增量变更（可序列化到
 * MANIFEST）
 */

#pragma once

#include "db/dbformat.h" // SequenceNumber, InternalKey 相关
#include "mapledb/slice.h"
#include "mapledb/status.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mapledb {

// ════════════════════════════════════════════════════════
// FileMetaData：描述磁盘上一个 SSTable 文件
// ════════════════════════════════════════════════════════
struct FileMetaData {
    uint64_t number;    // 文件编号（全局唯一，文件名 = number.ldb）
    uint64_t file_size; // 文件大小（字节）

    // 该 SSTable 中最小和最大的 InternalKey
    // 用字符串存储（已编码的 InternalKey 字节串）
    std::string smallest; // 最小 InternalKey（编码后）
    std::string largest;  // 最大 InternalKey（编码后）

    // 引用计数（VersionSet 管理生命周期）
    int refs = 0;

    FileMetaData() : number(0), file_size(0), refs(0) {}
};

// ════════════════════════════════════════════════════════
// VersionEdit：一次原子的文件集合变更
//
// 序列化格式（TLV：tag-value 对，每个 tag 是 varint32）：
//   kComparator      = 1  → comparator 名称
//   kLogNumber       = 2  → WAL 文件号
//   kNextFileNumber  = 3  → 下一个可用文件号
//   kLastSequence    = 4  → 最后序列号
//   kDeletedFile     = 6  → 删除的文件（level + file_number）
//   kNewFile         = 7  → 新增的文件（level + FileMetaData）
// ════════════════════════════════════════════════════════
class VersionEdit {
public:
    VersionEdit() { Clear(); }

    void Clear();

    // ── Setter：设置各字段 ───────────────────────────────────

    void SetComparatorName(const Slice& name) {
        has_comparator_ = true;
        comparator_     = name.ToString();
    }
    void SetLogNumber(uint64_t num) {
        has_log_number_ = true;
        log_number_     = num;
    }
    void SetNextFile(uint64_t num) {
        has_next_file_number_ = true;
        next_file_number_     = num;
    }
    void SetLastSequence(SequenceNumber seq) {
        has_last_sequence_ = true;
        last_sequence_     = seq;
    }

    // 新增一个文件（Compaction 完成时或 MemTable flush 时调用）
    //   level    : 目标层级
    //   file_num : 文件编号
    //   size     : 文件大小（字节）
    //   smallest : 最小 InternalKey（编码后的字符串）
    //   largest  : 最大 InternalKey（编码后的字符串）
    void AddFile(int level, uint64_t file_num, uint64_t size, const std::string& smallest,
                 const std::string& largest) {
        FileMetaData f;
        f.number    = file_num;
        f.file_size = size;
        f.smallest  = smallest;
        f.largest   = largest;
        new_files_.emplace_back(level, f);
    }

    // 标记删除一个文件（Compaction 输入文件）
    void DeleteFile(int level, uint64_t file_num) { deleted_files_.insert({level, file_num}); }

    // ── 序列化/反序列化 ─────────────────────────────────────

    // 将 VersionEdit 序列化为字节串（追加到 dst）
    void EncodeTo(std::string* dst) const;

    // 从字节串反序列化
    Status DecodeFrom(const Slice& src);

    // ── Getter（供 VersionSet 读取）──────────────────────────

    bool has_comparator() const { return has_comparator_; }
    bool has_log_number() const { return has_log_number_; }
    bool has_next_file_number() const { return has_next_file_number_; }
    bool has_last_sequence() const { return has_last_sequence_; }

    const std::string& comparator() const { return comparator_; }
    uint64_t           log_number() const { return log_number_; }
    uint64_t           next_file_number() const { return next_file_number_; }
    SequenceNumber     last_sequence() const { return last_sequence_; }

    const std::vector<std::pair<int, FileMetaData>>& new_files() const { return new_files_; }
    const std::set<std::pair<int, uint64_t>>& deleted_files() const { return deleted_files_; }

private:
    // MANIFEST 序列化 Tag 枚举
    enum Tag {
        kComparator     = 1,
        kLogNumber      = 2,
        kNextFileNumber = 3,
        kLastSequence   = 4,
        kDeletedFile    = 6,
        kNewFile        = 7,
    };

    std::string    comparator_;
    uint64_t       log_number_;
    uint64_t       next_file_number_;
    SequenceNumber last_sequence_;

    bool has_comparator_;
    bool has_log_number_;
    bool has_next_file_number_;
    bool has_last_sequence_;

    // 新增文件列表：(level, FileMetaData)
    std::vector<std::pair<int, FileMetaData>> new_files_;

    // 删除文件集合：(level, file_number)
    std::set<std::pair<int, uint64_t>> deleted_files_;
};

} // namespace mapledb