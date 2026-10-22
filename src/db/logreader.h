/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-05 15:19:51
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-05 15:20:13
 * @Description: WAL 日志读取器 崩溃恢复时，顺序读出 WAL 中的每条逻辑 Record（WriteBatch 的 rep_）
 */
// src/db/log_reader.h
// WAL 日志读取器
// 崩溃恢复时，顺序读出 WAL 中的每条逻辑 Record（WriteBatch 的 rep_）

#pragma once

#include "db/logformat.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"

#include <cstdint>
#include <string>

namespace mapledb {
namespace log {

class Reader {
public:
    // 错误报告接口（可选，默认忽略）
    class Reporter {
    public:
        virtual ~Reporter() = default;
        // 遇到损坏数据时调用（bytes 是丢弃的字节数，reason 是原因）
        virtual void Corruption(size_t bytes, const Status& reason) = 0;
    };

    // src_filename：要读取的 WAL 文件路径
    // reporter：错误报告器（可为 nullptr）
    // checksum：是否校验 CRC32
    explicit Reader(const std::string& src_filename, Reporter* reporter = nullptr,
                    bool checksum = true);

    ~Reader();

    Reader(const Reader&)            = delete;
    Reader& operator=(const Reader&) = delete;

    // 读取下一条逻辑 Record
    // record：输出参数，指向读出的数据（存在 scratch 里）
    // scratch：用于拼接跨 Block 分片的中间缓冲区
    // 返回 true：成功读到一条完整记录
    // 返回 false：文件结束或遇到不可恢复的错误
    bool ReadRecord(Slice* record, std::string* scratch);
    Status status() const { return status_; } // EOF 返回 false，错误必须另行检查，不能混为正常结束。

    // 当前读取位置（字节偏移，用于调试）
    uint64_t LastRecordOffset() const { return last_record_offset_; }
    // 最后一条完整逻辑记录的结束位置；恢复追加时必须丢弃此位置之后的残尾。
    uint64_t LastRecordEndOffset() const { return last_record_end_offset_; }

private:
    // 读一条物理 Record（可能是 FULL/FIRST/MIDDLE/LAST 之一）
    // 返回 RecordType，或 kEof / kBadRecord
    unsigned int ReadPhysicalRecord(Slice* result);

    static constexpr unsigned int kEof       = kMaxRecordType + 1;
    static constexpr unsigned int kBadRecord = kMaxRecordType + 2;

    void ReportCorruption(size_t bytes, const char* reason);

    FILE*     src_; // WAL 文件句柄（只读）
    Reporter* reporter_;
    bool      checksum_;
    char*     backing_store_;        // 存放当前 Block 数据的缓冲区
    Slice     buffer_;               // 当前 Block 的可读视图
    bool      eof_;                  // 是否已到文件末尾
    Status    status_;               // 保留第一次打开、读取或格式错误。
    uint64_t  last_record_offset_;   // 上一条完整记录的起始偏移
    uint64_t  last_record_end_offset_ = 0;
    uint64_t  end_of_buffer_offset_; // buffer_ 末尾对应的文件偏移
};

} // namespace log
} // namespace mapledb
