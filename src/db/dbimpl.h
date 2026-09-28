/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-17 11:18:17
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-28 08:53:44
 * @Description: src/db/db_impl.h
 * DBImpl：DB 接口的核心实现
 * 持有所有子系统（WAL、MemTable、VersionSet），串联读写流程
 */
#if 1

    #pragma once

    #include "db/dbformat.h"
    #include "db/dbiterator.h"
    #include "db/logreader.h"
    #include "db/logwriter.h"
    #include "db/memtable.h"
    #include "db/memtableiterator.h"
    #include "db/mergeiterator.h"
    #include "db/versionedit.h"
    #include "db/versionset.h"
    #include "db/writebatch.h"
    #include "mapledb/db.h"
    #include "mapledb/options.h"
    #include "mapledb/slice.h"
    #include "mapledb/status.h"
    #include "table/table.h"
    #include "table/tablebuilder.h"

    #include <condition_variable>
    #include <cstdint>
    #include <mutex>
    #include <string>
    #include <thread>

namespace mapledb {
class TableCache;
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
    Status Flush() override;

    // ── 内部初始化（由 DB::Open 调用）────────────────────────
    Status Open();

    Iterator* NewIterator() override;

private:
    // ── Major Compaction 描述 ────────────────────────────────────
    // 描述一次 Major Compaction 的输入文件和输出文件
    struct Compaction {
        int                        level;     // 被合并的层（输出到 level+1）
        std::vector<FileMetaData*> inputs[2]; // [0]=level层文件, [1]=level+1层文件
        std::vector<FileMetaData*> outputs;   // 合并产生的新文件（在 level+1）
        ~Compaction() {
            for (auto* f : outputs)
                delete f;
        }
    };
    // ── 写路径 ────────────────────────────────────────────────

    // 以 WriteBatch 写入（Put/Delete 的底层实现）
    Status Write(WriteBatch* batch);

    // 确保 mem_ 有足够空间接收新写入
    // MemTable 满时：切换到新 MemTable，触发同步 Flush
    Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);
    Status RotateMemTable(); // 必须持 mutex_

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

    // 返回第 level 层的大小上限（L1=10MB, L2=100MB, ...）
    static uint64_t LevelMaxBytes(int level);

    // 检查是否需要触发 Major Compaction，返回需要合并的层
    bool NeedsCompaction(int* level) const;

    // 选出需要合并的文件，返回值由调用方 delete
    Compaction* PickCompaction();

    // 多路归并主循环：读输入文件，去重，写出新 SSTable
    Status DoCompactionWork(Compaction* c);

    // 将合并结果原子写入 VersionSet
    Status InstallCompactionResults(Compaction* c);

    // 删除磁盘上已不在 VersionSet 中的旧 SSTable 文件
    void RemoveObsoleteFiles();

    // Day 12：只负责判断并调度（通知后台线程）
    // 必须在持有 mutex_ 时调用
    void MaybeScheduleCompaction();

    // ── Day 12: 后台 Compaction 线程 ─────────────────────────────
    void BGWork();               // 后台线程入口（循环等待 + 执行）
    void BackgroundCompaction(); // Compaction 主体（无锁调用）
    void ScheduleCompaction();   // 通知后台线程（必须持 mutex_）

    // ── 成员变量 ──────────────────────────────────────────────
    const std::string     dbname_;
    const Options         options_;
    InternalKeyComparator icmp_;

    MemTable* mem_; // 当前可写 MemTable
    MemTable* imm_; // 不可变 MemTable（等待 Flush）

    log::Writer* log_writer_; // 当前 WAL 写入器
    uint64_t     log_number_; // 当前 WAL 文件号

    VersionSet versions_; // SSTable 文件管理

    TableCache* table_cache_;

    mutable std::mutex      mutex_;         // 保护所有共享状态
    std::condition_variable bg_cv_;         // 后台线程等待 / 写入者等待
    bool                    bg_scheduled_;  // 后台 Compaction 是否已调度
    bool                    bg_running_ = false;
    bool                    shutting_down_; // DB 正在关闭
    Status                  bg_error_;      // 后台线程遇到的错误
    std::thread             bg_thread_;     // 后台 Compaction 线程
};

} // namespace mapledb

#endif
