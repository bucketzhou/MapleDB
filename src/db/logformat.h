/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-05 13:54:50
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-05 13:55:06
 * @Description: WAL 日志文件的格式常量定义 参考 LevelDB doc/log_format.md
 */

#pragma once
#include <cstdint>

namespace mapledb {
namespace log {

// 每个物理 Block 的大小（32KB，与操作系统 I/O 对齐友好）
static constexpr uint32_t kBlockSize = 32768;

// 每条物理 Record 的头部大小：
//   CRC32(4B) + length(2B) + type(1B) = 7 字节
static constexpr int kHeaderSize = 4 + 2 + 1;

// Record 类型（type 字段，1 字节）
enum RecordType : uint8_t {
    kZeroType   = 0, // 预分配文件的填充，读取时跳过
    kFullType   = 1, // 完整记录（数据在一个 Block 内）
    kFirstType  = 2, // 跨 Block 分片的第一段
    kMiddleType = 3, // 跨 Block 分片的中间段
    kLastType   = 4, // 跨 Block 分片的最后一段
};

static constexpr RecordType kMaxRecordType = kLastType;

} // namespace log
} // namespace mapledb