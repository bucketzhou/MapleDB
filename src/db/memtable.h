/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-03 16:40:29
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-27 14:27:42
 * @Description: MemTable：基于 SkipList 的内存键值存储
 *职责：
 * - 接受写操作（Put/Delete），编码成 InternalKey 格式存入 SkipList
 * - 接受读操作（Get），通过 LookupKey + Seek 找到最新版本
 * - 提供迭代器（用于 flush 到 SSTable）
 * - 用引用计数管理生命周期（MemTable 可能同时被写入线程和 flush 线程引用）
 */
#pragma once

#include "dbformat.h"
#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "skiplist.h"
#include "util/arena.h"

#include <atomic> // 新增
#include <string>

namespace mapledb {
class MemTable {
public:
    // SkipList 的 key 类型是 const char*（带 varint 前缀的 memtable key）
    // Comparator 是 InternalKeyComparator
    using Table = SkipList<const char*, InternalKeyComparator>;

    explicit MemTable(const InternalKeyComparator& comparator);
    // 禁止拷贝（含有 Arena 和 SkipList，拷贝语义复杂且无必要）
    MemTable(const MemTable&)            = delete;
    MemTable& operator=(const MemTable&) = delete;
    // ── 引用计数（生命周期管理）──────────────────────────────
    // MemTable 可能同时被：
    //   1. 写入线程（通过 DBImpl::mem_）引用
    //   2. flush 线程（通过 DBImpl::imm_）引用
    // 引用计数降为 0 时，MemTable 自我销毁

    void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
    void Unref() {
        int r = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        assert(r >= 0);
        if (r == 0)
            delete this;
    }

    // ── 核心接口 ─────────────────────────────────────────────

    // 写入一条记录
    // seq：当前操作的序列号（由 DB 层维护，每次写操作递增）
    // type：kTypeValue（Put）或 kTypeDeletion（Delete）
    // key/value：用户提供的 key 和 value
    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

    // 查询一条记录
    // key：LookupKey（包含 user_key 和查询时的 SequenceNumber）
    // value：查询成功时，value 写入这里
    // 返回 true 表示找到（包括找到删除标记，此时 Status 为 NotFound）
    // 返回 false 表示 key 不在此 MemTable 中
    bool Get(const LookupKey& key, std::string* value, Status* s) const;

    // 返回 MemTable 使用的内存量（字节）
    // DBImpl 用这个值判断是否需要转换为 Immutable MemTable
    size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

    // ── 迭代器（用于 flush 时顺序读取所有 kv）────────────────
    // 返回的迭代器遍历所有 entry，顺序为 InternalKey 升序
    // 调用方负责 delete 返回的迭代器
    Table::Iterator* NewIterator();

private:
    ~MemTable(); // private：通过引用计数自动释放，不允许外部 delete

    InternalKeyComparator comparator_;
    std::atomic<int>      refs_;  // atomic<int> 引用计数（初始为 0，第一次 Ref() 后变为 1）
    Arena                 arena_; // 所有节点内存从这里分配
    Table                 table_; // 底层 SkipList
};
} // namespace mapledb