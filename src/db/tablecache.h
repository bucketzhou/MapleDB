/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-21 07:55:46
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-22 11:11:02
 * @Description:
 * TableCache：缓存打开的 SSTable 文件（Table 对象）功能：
 * - 以文件号（uint64_t）为 key，缓存 Table* 对象
 * - 缓存命中时直接返回已打开的 Table，避免重复 fopen / 读 Index Block
 * - 支持按文件号 Evict（删除旧文件时调用）
 * 参考 LevelDB table_cache.h
 */

#pragma once

#include "db/versionedit.h" // FileMetaData
#include "mapledb/options.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "table/table.h"
#include "util/cache.h"
#include "util/comparator.h" // Comparator

#include <string>
#include <mutex>

namespace mapledb {

class TableCache {
public:
    // dbname  : DB 目录路径
    // options : DB 配置（含 block_cache 等）
    // cmp     : 用于打开 Table 的比较器（通常是 InternalKeyComparator*）
    // entries : Table 缓存最多可容纳的条目数
    TableCache(const std::string& dbname, const Options& options, const Comparator* cmp,
               int entries);
    ~TableCache();

    TableCache(const TableCache&)            = delete;
    TableCache& operator=(const TableCache&) = delete;

    // 返回指定文件的迭代器（调用方负责 delete）
    // 若 tableptr != nullptr，则同时通过 *tableptr 返回 Table 指针（弱引用，不转移所有权）
    // 迭代器持有缓存句柄，淘汰不会使正在使用的 Table 失效。
    Table::Iterator* NewIterator(const FileMetaData* meta, Table** tableptr = nullptr);

    // 在指定 SSTable 中查找 internal_key。
    // 找到时调用 handle_result(arg, key, value)。
    // 未找到时返回 OK 但不调用回调。
    Status Get(const FileMetaData* meta, const Slice& internal_key, void* arg,
               void (*handle_result)(void*, const Slice&, const Slice&));

    // 从缓存中淘汰指定文件号的条目（在删除 SST 文件前调用）
    void Evict(uint64_t file_number);

private:
    // 在缓存中查找 file_number 对应的 Table；未命中时打开文件并插入。
    // 成功时 *handle 持有引用，调用方完成后必须 cache_->Release(*handle)。
    Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle);

    // 生成 SST 文件路径：dbname_/NNNNNN.ldb
    std::string TableFileName(uint64_t number) const;

    std::string       dbname_;
    Options           options_;
    const Comparator* cmp_;   // 比较器（不拥有）
    Cache*            cache_; // LRU Cache 实例（拥有）
    std::mutex        open_mutex_; // 同一缓存未命中只打开一次
};

} // namespace mapledb
