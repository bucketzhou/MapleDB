/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-04 16:28:22
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-04 16:32:24
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