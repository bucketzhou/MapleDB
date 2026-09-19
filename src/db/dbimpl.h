/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-17 11:18:17
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-17 11:20:50
 * @Description: src/db/db_impl.h
 * DBImpl：DB 接口的核心实现
 * 持有所有子系统（WAL、MemTable、VersionSet），串联读写流程
 */
#if 1

    #pragma once

    #include "db/dbformat.h"
    #include "db/logreader.h"
    #include "db/logwriter.h"
    #include "db/memtable.h"
    #include "db/versionedit.h"
    #include "db/versionset.h"
    #include "db/writebatch.h"
    #include "mapledb/db.h"
    #include "mapledb/options.h"
    #include "mapledb/slice.h"
    #include "mapledb/status.h"
    #include "table/table.h"
    #include "table/tablebuilder.h"

    #include <cstdint>
    #include <string>

namespace mapledb {

class DBImpl : public DB {
public:
    DBImpl(const Options& options, const std::string& dbname);
    ~DBImpl() override;

    DBImpl(const DBImpl&)            = delete;
    DBImpl& operator=(const DBImpl&) = delete;

    // ── DB 接口实现 ───────────────────────────────────────────
    Status Put(const std::string& key, const std::string& value) override;
    Status Delete(const std::string& key) override;
    Status Get(const std::string& key, std::string* value) override;

    // ── 内部初始化（由 DB::Open 调用）────────────────────────
    Status Open();

private:
    // ── 写路径 ────────────────────────────────────────────────

    // 以 WriteBatch 写入（Put/Delete 的底层实现）
    Status Write(WriteBatch* batch);

    // 确保 mem_ 有足够空间接收新写入
    // MemTable 满时：切换到新 MemTable，触发同步 Flush
    Status MakeRoomForWrite();

    // ── Minor Compaction（MemTable Flush）────────────────────

    // 将 imm_ flush 到 Level 0 SSTable
    Status CompactMemTable();

    // 遍历 imm_ 的所有 entry，写成一个 SSTable 文件
    // 填充 edit（AddFile）和 meta（文件号、大小、key 范围）
    Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, uint64_t file_number,
                            std::string* smallest_key, std::string* largest_key,
                            uint64_t* file_size);

    // ── 读路径 ────────────────────────────────────────────────

    // 在单个 SSTable 文件里查找 internal_key
    // 找到时通过回调返回结果
    Status SearchSST(const FileMetaData* file, const Slice& internal_key, std::string* value,
                     bool* found);

    // ── 恢复（DB::Open 内调用）───────────────────────────────

    // 重放一个 WAL 文件，将数据写回 mem_
    Status RecoverLogFile(uint64_t log_number, SequenceNumber* max_sequence);

    // ── SSTable 文件路径工具 ──────────────────────────────────
    std::string TableFileName(uint64_t number) const;
    std::string LogFileName(uint64_t number) const;

    // ── 成员变量 ──────────────────────────────────────────────
    const std::string     dbname_;
    const Options         options_;
    InternalKeyComparator icmp_;

    MemTable* mem_; // 当前可写 MemTable
    MemTable* imm_; // 不可变 MemTable（等待 Flush）

    log::Writer* log_writer_; // 当前 WAL 写入器
    uint64_t     log_number_; // 当前 WAL 文件号

    VersionSet versions_; // SSTable 文件管理
};

} // namespace mapledb

#endif