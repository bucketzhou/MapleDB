/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-05 13:57:15
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-05 13:57:53
 * @Description: WAL 日志写入器, 负责将 WriteBatch 的二进制内容作为 Record 顺序追加到 WAL 文件
 */

#pragma once

#include "db/logformat.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"

#include <cstdint>
#include <string>

namespace mapledb {
namespace log {

class Writer {
public:
    // dest：WAL 文件路径（文件会被创建或追加）
    // 构造时会打开或创建文件（追加模式）
    explicit Writer(const std::string& dest_filename);

    ~Writer();

    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    // 将 slice 作为一条逻辑 Record 追加到 WAL 文件
    // 内部处理跨 Block 分片
    // sync=true：写后调用 fsync 强制刷盘（性能 vs 安全的权衡）
    Status AddRecord(const Slice& slice, bool sync = false);

    // 强制将内核缓冲区刷到磁盘（比每次 sync 更灵活）
    Status Flush();

private:
    // 写一条物理 Record（可能是 FULL / FIRST / MIDDLE / LAST）
    Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

    FILE*    dest_;         // WAL 文件句柄
    uint32_t block_offset_; // 当前 Block 内已写字节数（0..kBlockSize-1）

    // CRC32 计算（用 type 做种，这样不同 type 的相同内容 CRC 不同）
    // 我们用标准 CRC32 简化实现（LevelDB 用 crc32c，效果类似）
    uint32_t ComputeCRC(RecordType type, const char* data, size_t n) const;
};

} // namespace log
} // namespace mapledb