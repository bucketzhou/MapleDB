/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-04 16:28:22
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-17 12:59:35
 * @Description:
 *   WriteBatch：原子批量写操作的载体
 *   内部用一段二进制 rep_ 序列化所有操作，可以：
 *   1. 作为一个整体写入 WAL（原子性）
 *   2. 被 InsertInto 应用到 MemTable
 */

#pragma once

#include "mapledb/slice.h"
#include "mapledb/status.h"

#include <string>
namespace mapledb {

class MemTable;

class WriteBatch {
public:
    // ── Handler：访问者模式，解耦 batch 和具体存储 ───────────
    class Handler {
    public:
        virtual ~Handler()                                     = default;
        virtual void Put(const Slice& key, const Slice& value) = 0;
        virtual void Delete(const Slice& key)                  = 0;
    };

    WriteBatch();
    ~WriteBatch() = default;

    WriteBatch(const WriteBatch&)            = delete;
    WriteBatch& operator=(const WriteBatch&) = delete;

    // ── 构建接口 ──────────────────────────────────────────────
    void Put(const Slice& key, const Slice& value);
    void Delete(const Slice& key);
    void Clear();
    void Append(const WriteBatch& source);

    // ── 消费接口 ──────────────────────────────────────────────
    Status Iterate(Handler* handler) const;
    Status InsertInto(MemTable* memtable) const;

    // ── 内部状态访问（供 WAL 序列化使用）─────────────────────
    Slice    Contents() const { return Slice(rep_); }
    void     SetSequence(uint64_t seq);
    uint64_t Sequence() const;
    int      Count() const;
    size_t   ByteSize() const { return rep_.size(); }
    void     DecodeFrom(const Slice& contents);

    // rep_ 头部大小（供 DBImpl 做最小长度检查）
    static constexpr size_t kHeader = 12;

private:
    friend class WriteBatchInternal;

    // rep_ 内存布局：
    //   [0..7]  : sequence (fixed64 LE) — batch 起始序列号
    //   [8..11] : count    (fixed32 LE) — batch 内操作数量
    //   [12..]  : records               — 各操作的编码
    std::string rep_;
};

// ════════════════════════════════════════════════════════
// WriteBatchInternal：DB 内部使用的静态辅助类
//
// DBImpl 需要通过静态方法操作 WriteBatch，而不是直接调用
// WriteBatch 的成员方法（保持接口层次清晰）。
// 全部 inline 实现，无需额外 .cpp。
// ════════════════════════════════════════════════════════
class WriteBatchInternal {
public:
    // rep_ 头部大小，供外部做长度检查
    static constexpr size_t kHeader = WriteBatch::kHeader;

    // ── 序列号操作 ────────────────────────────────────────────

    // 获取 batch 的起始序列号
    static uint64_t Sequence(const WriteBatch* batch) { return batch->Sequence(); }

    // 设置 batch 的起始序列号（DB 层在提交写入前调用）
    static void SetSequence(WriteBatch* batch, uint64_t seq) { batch->SetSequence(seq); }

    // ── 操作计数 ──────────────────────────────────────────────

    // 返回 batch 中操作的条数
    static int Count(const WriteBatch* batch) { return batch->Count(); }

    // ── 序列化内容 ────────────────────────────────────────────

    // 返回 rep_ 的 Slice 视图（用于写入 WAL）
    static Slice Contents(const WriteBatch* batch) { return batch->Contents(); }

    // 用已序列化的 Slice 覆盖 batch 内容（从 WAL 读回时调用）
    static void SetContents(WriteBatch* batch, const Slice& contents) {
        batch->DecodeFrom(contents);
    }

    // ── MemTable 写入 ─────────────────────────────────────────

    // 将 batch 的所有操作应用到 memtable
    static Status InsertInto(const WriteBatch* batch, MemTable* memtable) {
        return batch->InsertInto(memtable);
    }
};

} // namespace mapledb

#if 0


namespace mapledb {

class MemTable;

class WriteBatch {
public:
    // ── Handler：访问者模式，解耦 batch 和具体存储 ───────────
    // 使用者继承 Handler，实现 Put/Delete，
    // 调用 Iterate(handler) 时会回调这两个方法
    class Handler {
    public:
        virtual ~Handler()                                     = default;
        virtual void Put(const Slice& key, const Slice& value) = 0;
        virtual void Delete(const Slice& key)                  = 0;
    };

    WriteBatch();
    ~WriteBatch() = default;

    // 禁止拷贝（提倡传引用，避免 rep_ string 的大量拷贝）
    WriteBatch(const WriteBatch&)            = delete;
    WriteBatch& operator=(const WriteBatch&) = delete;

    // ── 构建接口 ──────────────────────────────────────────────

    // 追加一条 Put 操作（kTypeValue）
    void Put(const Slice& key, const Slice& value);

    // 追加一条 Delete 操作（kTypeDeletion，以墓碑方式记录）
    void Delete(const Slice& key);

    // 清空所有操作，重置为初始状态
    void Clear();

    // 将 source 中的所有操作追加到 this（合并 batch）
    void Append(const WriteBatch& source);

    // ── 消费接口 ──────────────────────────────────────────────

    // 遍历 batch 中的每条操作，回调 handler 的 Put/Delete
    // 返回 Corruption 如果 rep_ 格式有误
    Status Iterate(Handler* handler) const;

    // 将 batch 中的所有操作直接应用到 memtable
    // 这是最常用的消费方式（从 WAL 恢复时也走这里）
    Status InsertInto(MemTable* memtable) const;

    // ── 内部状态访问（供 WAL 序列化使用）─────────────────────

    // 返回 rep_ 的 Slice 视图（用于写入 WAL）
    Slice Contents() const { return Slice(rep_); }

    // 设置 batch 起始的 SequenceNumber（DB层在提交时设置）
    void SetSequence(uint64_t seq);

    // 获取 batch 起始的 SequenceNumber
    uint64_t Sequence() const;

    // 返回 batch 中操作的条数
    int Count() const;

    // 返回 rep_ 的字节大小（WAL 写入时用于计算 record 长度）
    size_t ByteSize() const { return rep_.size(); }

    // 从已序列化的 Slice（WAL 读出的 record）反序列化
    void DecodeFrom(const Slice& contents);

private:
    // rep_ 内存布局：
    //   [0..7]  : sequence (fixed64 LE) — batch 起始序列号
    //   [8..11] : count    (fixed32 LE) — batch 内操作数量
    //   [12..]  : records               — 各操作的编码
    std::string rep_;

    // kHeader：rep_ 头部占用的字节数
    static constexpr size_t kHeader = 12;
};
} // namespace mapledb
#endif