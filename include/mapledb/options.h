/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-23 13:30:59
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-23 14:10:34
 * @Description:
 */
#pragma once
#include <cstddef>
#include <cstdint>
namespace mapledb {
struct Options {
    /* -------------------------------- MemTable -------------------------------- */
    // 写满多少字节后触发flush，参考levelDB，4mb
    size_t writer_buffer_size = 4 * 1024 * 1024;
    /* --------------------------------- SSTable -------------------------------- */
    // 每个SSTable 数据块的大小
    size_t block_size = 4 * 1024;

    // SSTable 最大文件大小（默认 2MB，参考 LevelDB）
    size_t max_file_size = 2 * 1024 * 1024;

    /* ------------------------------ Compaction 相关 ----------------------------- */
    // Level-0 文件数超过此值触发 compaction
    int l0_compaction_trigger = 4;

    /* ---------------------------------- 布隆过滤器 --------------------------------- */
    // 每个 key 使用的 bloom filter 比特数（10 bits ≈ 1% 误判率）
    int bloom_bits_per_key = 10;

    /* ----------------------------------- 通用 ----------------------------------- */
    // 如果 DB 目录不存在，是否自动创建
    bool create_if_missing = true;
};
} // namespace mapledb