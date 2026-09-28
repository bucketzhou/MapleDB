/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-12 13:28:00
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-28 07:56:09
 * @Description: src/table/table.h
 * Table：SSTable 文件在内存中的读取接口
 * 提供 Open（打开文件）/ InternalGet（点查）/ NewIterator（全扫描）
 * 修复说明:
 * ReadBlock 从 private 改为 public，供 TwoLevelIterator（table.cpp 内部类）合法访问。
 */

#pragma once

#include "mapledb/iterator.h"
#include "mapledb/options.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "table/block.h" // 包含完整的 Block + Block::Iter 定义
#include "table/format.h"
#include "table/filter_block.h"
#include "util/cache.h"

#include <cstdio>
#include <string>
#include <memory>

namespace mapledb {

class Table {
public:
    // 打开一个 SSTable 文件
    //   filename : 文件路径
    //   options  : DB 配置
    //   cmp      : 用于 Block 内比较的 Comparator
    //              存 InternalKey 的 SSTable 传 InternalKeyComparator
    //              存普通 key 的 SSTable 传 BytewiseComparator()（默认）
    //   table    : 成功时输出新建的 Table 对象（调用方负责 delete）
    static Status Open(const std::string& filename, const Options& options, const Comparator* cmp,
                       Table** table);

    // 兼容旧接口：不传 cmp 时默认用 BytewiseComparator
    static Status Open(const std::string& filename, const Options& options, Table** table) {
        return Open(filename, options, BytewiseComparator(), table);
    }

    ~Table();

    Table(const Table&)            = delete;
    Table& operator=(const Table&) = delete;

    // 点查：在 SSTable 中查找 key
    //   找到时调用 handle_result(arg, found_key, found_value)
    //   未找到时返回 OK 但不调用回调
    Status InternalGet(const Slice& key, void* arg,
                       void (*handle_result)(void*, const Slice&, const Slice&));

    // ── Iterator 抽象接口 ─────────────────────────────────────
    // 遍历整个 SSTable 的 kv（两级迭代器：Index Block → DataBlock）
    // struct Iterator {
    //     virtual ~Iterator()                            = default;
    //     virtual bool         Valid() const             = 0;
    //     virtual void         SeekToFirst()             = 0;
    //     virtual void         Seek(const Slice& target) = 0;
    //     virtual void         Next()                    = 0;
    //     virtual const Slice& key() const               = 0;
    //     virtual const Slice& value() const             = 0;
    //     virtual Status       status() const            = 0;
    // };
    using Iterator = mapledb::Iterator;

    // 创建迭代器（调用方负责 delete）
    Iterator* NewIterator();

    // 文件大小（字节）
    uint64_t FileSize() const { return file_size_; }

    // ── ReadBlock：public（供 TwoLevelIterator 使用）──────────
    // 从文件中读取一个 Block（根据 handle 定位）
    // 传 cache_handle 时必须通过 ReleaseBlock 释放，不可直接 delete 缓存 Block。
    // 不传 cache_handle 时不使用缓存，返回的 Block 由调用方 delete。
    Status ReadBlock(const BlockHandle& handle, Block** block,
                     Cache::Handle** cache_handle = nullptr) const;
    void ReleaseBlock(Block* block, Cache::Handle* cache_handle) const;
    const Comparator* ComparatorForIterator() const { return rep_->cmp; }

private:
    // Table 的内部数据
    struct Rep {
        FILE*             file;
        uint64_t          file_size;
        Block*            index_block; // Index Block 常驻内存，避免每次查询都重读
        Options           options;
        BlockHandle       metaindex_handle;
        const Comparator* cmp;      // ← 新增：存储传入的比较器
        uint64_t          cache_id; // Day 11 新增：BlockCache 分区 ID（0 表示未启用）
        std::string filter_data; // 常驻 Table，不占用 DataBlock LRU 配额。
        std::unique_ptr<FilterBlockReader> filter;
        FilterKeyMode filter_key_mode = FilterKeyMode::kUserKey;
    };

    void ReadFilter(); // 可选 metadata 不可用时回退到正常查询。

    explicit Table(Rep* rep) : rep_(rep), file_size_(rep->file_size) {}

    Rep*     rep_;
    uint64_t file_size_;
};

} // namespace mapledb
