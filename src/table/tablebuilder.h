/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-11 16:27:18
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-11 18:35:05
 * @Description: TableBuilder：构建完整的 SSTable 文件  调用方按照 InternalKey 升序调用
 * Add()，完成后调用 Finish()
 */
#pragma once

#include "mapledb/options.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "table/format.h"

#include <cstdio>
#include <string>

namespace mapledb {

class TableBuilder {
public:
    // filename：SSTable 文件路径（会创建/覆盖）
    // options ：DB 配置（block_size 等）
    TableBuilder(const std::string& filename, const Options& options);

    ~TableBuilder();

    TableBuilder(const TableBuilder&)            = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    // 追加一条 kv（key 必须严格递增）
    // key 是 InternalKey（user_key + seq + type）
    void Add(const Slice& key, const Slice& value);

    // 完成文件写入（写 MetaIndex Block、Index Block、Footer）
    // 调用后文件已完整，可以关闭
    Status Finish();

    // 放弃当前构建（出错时使用）
    void Abandon();

    // 已写入的 kv 条数
    uint64_t NumEntries() const { return num_entries_; }

    // 文件当前大小（字节，用于判断是否超过 max_file_size）
    uint64_t FileSize() const { return offset_; }

    // 是否有错误
    Status status() const { return status_; }

private:
    // 把当前 data_block_ 刷入文件，更新 Index Block
    void Flush();

    // 把一个 Block（BlockBuilder 的输出）写入文件，填充 handle
    // 末尾追加 1 字节 type + 4 字节 CRC32
    void WriteBlock(const Slice& block_contents, BlockHandle* handle);

    // 计算 CRC32（复用同一个简单实现）
    uint32_t ComputeCRC(const char* data, size_t n) const;

    // ── 内部状态 ─────────────────────────────────────────────
    Options  options_;
    FILE*    file_;        // 输出文件句柄
    uint64_t offset_;      // 当前文件写指针（字节偏移）
    Status   status_;      // 最近一次 I/O 操作的状态
    uint64_t num_entries_; // 已写入 kv 数量
    bool     closed_;      // 是否已调用 Finish/Abandon

    // ── Block 构建器 ─────────────────────────────────────────
    // 使用前向声明避免 block_builder.h 的循环依赖
    struct BlockBuilderWrapper;
    BlockBuilderWrapper* data_block_  = nullptr; // 当前正在构建的 Data Block
    BlockBuilderWrapper* index_block_ = nullptr; // Index Block（整个文件只有一个）

    // ── 待写入 Index Block 的信息 ─────────────────────────────
    bool        pending_index_entry_; // 上一个 block flush 完，等待写 index entry
    BlockHandle pending_handle_;      // 上一个 block 的 handle
    std::string last_key_;            // 上一个写入的 key（用于生成 index key）
};
} // namespace mapledb