/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-23 13:30:59
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-21 07:57:29
 * @Description:
 */
#pragma once
#include <cstddef>
#include <cstdint>
namespace mapledb {

class Cache;

struct Options {
    /* -------------------------------- MemTable -------------------------------- */
    // 写满多少字节后触发flush，参考levelDB，4mb
    size_t writer_buffer_size = 4 * 1024 * 1024;
    /* --------------------------------- SSTable -------------------------------- */
    // 每个SSTable 数据块的大小
    size_t block_size             = 4 * 1024;
    size_t block_restart_interval = 16;

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

    // SSTable Data Block 缓存（可选）。
    // 若非 nullptr，Table::ReadBlock 会先查询此缓存再访问磁盘。
    // 缓存由调用方创建和拥有（DB 不负责 delete）。
    // 示例：options.block_cache = minidb::NewLRUCache(8 * 1024 * 1024); // 8MB
    Cache* block_cache = nullptr;
    // 同步写：WAL 内容落盘后才返回成功；默认仍使用原有异步写策略。
    bool sync_writes = false;
};
} // namespace mapledb