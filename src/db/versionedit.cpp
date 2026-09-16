// src/db/version_edit.cpp
// VersionEdit 的序列化/反序列化实现

#include "db/versionedit.h"
#include "util/coding.h"

#include <cassert>

namespace mapledb {

void VersionEdit::Clear() {
    comparator_.clear();
    log_number_           = 0;
    next_file_number_     = 0;
    last_sequence_        = 0;
    has_comparator_       = false;
    has_log_number_       = false;
    has_next_file_number_ = false;
    has_last_sequence_    = false;
    new_files_.clear();
    deleted_files_.clear();
}

// ── EncodeTo：序列化为字节串 ─────────────────────────────────

void VersionEdit::EncodeTo(std::string* dst) const {
    // 每个字段：先写 tag（varint32），再写 value
    if (has_comparator_) {
        PutVarint32(dst, kComparator);
        PutLengthPrefixedSlice(dst, Slice(comparator_));
    }
    if (has_log_number_) {
        PutVarint32(dst, kLogNumber);
        PutVarint64(dst, log_number_);
    }
    if (has_next_file_number_) {
        PutVarint32(dst, kNextFileNumber);
        PutVarint64(dst, next_file_number_);
    }
    if (has_last_sequence_) {
        PutVarint32(dst, kLastSequence);
        PutVarint64(dst, last_sequence_);
    }

    // 删除文件（顺序无关）
    for (const auto& [level, file_num] : deleted_files_) {
        PutVarint32(dst, kDeletedFile);
        PutVarint32(dst, static_cast<uint32_t>(level));
        PutVarint64(dst, file_num);
    }

    // 新增文件
    for (const auto& [level, f] : new_files_) {
        PutVarint32(dst, kNewFile);
        PutVarint32(dst, static_cast<uint32_t>(level));
        PutVarint64(dst, f.number);
        PutVarint64(dst, f.file_size);
        // smallest 和 largest 以 length-prefixed string 格式存储
        PutLengthPrefixedSlice(dst, Slice(f.smallest));
        PutLengthPrefixedSlice(dst, Slice(f.largest));
    }
}

// ── DecodeFrom：从字节串反序列化 ─────────────────────────────

Status VersionEdit::DecodeFrom(const Slice& src) {
    Clear();
    Slice       input = src;
    const char* msg   = nullptr;
    uint32_t    tag;

    while (msg == nullptr && GetVarint32(&input, &tag)) {
        switch (tag) {
            case kComparator: {
                Slice str;
                if (GetLengthPrefixedSlice(&input, &str)) {
                    comparator_     = str.ToString();
                    has_comparator_ = true;
                } else {
                    msg = "comparator name";
                }
                break;
            }
            case kLogNumber:
                if (GetVarint64(&input, &log_number_)) {
                    has_log_number_ = true;
                } else {
                    msg = "log number";
                }
                break;

            case kNextFileNumber:
                if (GetVarint64(&input, &next_file_number_)) {
                    has_next_file_number_ = true;
                } else {
                    msg = "next file number";
                }
                break;

            case kLastSequence:
                if (GetVarint64(&input, &last_sequence_)) {
                    has_last_sequence_ = true;
                } else {
                    msg = "last sequence";
                }
                break;

            case kDeletedFile: {
                uint32_t level;
                uint64_t file_num;
                if (GetVarint32(&input, &level) && GetVarint64(&input, &file_num)) {
                    deleted_files_.insert({static_cast<int>(level), file_num});
                } else {
                    msg = "deleted file";
                }
                break;
            }

            case kNewFile: {
                uint32_t     level;
                FileMetaData f;
                Slice        key_slice;
                if (GetVarint32(&input, &level) && GetVarint64(&input, &f.number)
                    && GetVarint64(&input, &f.file_size)
                    && GetLengthPrefixedSlice(&input, &key_slice)) {
                    f.smallest = key_slice.ToString();
                    if (GetLengthPrefixedSlice(&input, &key_slice)) {
                        f.largest = key_slice.ToString();
                        new_files_.emplace_back(static_cast<int>(level), f);
                    } else {
                        msg = "new file largest key";
                    }
                } else {
                    msg = "new file";
                }
                break;
            }

            default:
                msg = "unknown tag";
                break;
        }
    }

    if (msg != nullptr) {
        return Status::Corruption("VersionEdit decode: ", msg);
    }
    return Status::OK();
}

} // namespace mapledb