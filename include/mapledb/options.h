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
    // 每个 key 使用的 Bloom 比特数；>0 时生成并使用 SSTable 过滤器，<=0 关闭。
    // 默认 10 bits/key；已有无过滤器的 SSTable 仍可读取，合并重写后才生成过滤器。
    int bloom_bits_per_key = 10;

    /* ----------------------------------- 通用 ----------------------------------- */
    // 如果 DB 目录不存在，是否自动创建
    bool create_if_missing = true;

    // SSTable Data Block 缓存（可选）。
    // pread 路径先查询此缓存；映射路径复用操作系统页缓存，不重复缓存映射数据。
    // 缓存由调用方创建和拥有（DB 不负责 delete）。
    // 示例：options.block_cache = minidb::NewLRUCache(8 * 1024 * 1024); // 8MB
    Cache* block_cache = nullptr;
    // 修改：64 位默认只读映射不可变 SST；失败自动回退 pread，可关闭以使用原块缓存路径。
    bool use_mmap_reads = true;
    // 同步写：WAL 内容落盘后才返回成功；默认仍使用原有异步写策略。
    bool sync_writes = false;
};
} // namespace mapledb
