/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-09 05:21:32
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-11 17:04:01
 * @Description: 构建一个 Block 的字节流（Data Block / Index Block 通用） 核心特性：前缀压缩 +
 * Restart Point
 */
#pragma once

#include "mapledb/slice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mapledb {
struct Options;
class BlockBuilder {
public:
    // block_restart_interval：每隔多少个 key 设置一个 Restart Point（默认 16）
    explicit BlockBuilder(int block_restart_interval = 16);

    BlockBuilder(const BlockBuilder&)            = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    // 重置为空状态（可复用，避免重复构造析构）
    void Reset();

    // 追加一条 kv 记录
    // 要求：key 必须严格大于上一次 Add 的 key（有序插入）
    void Add(const Slice& key, const Slice& value);

    // 完成构建，返回 Block 的完整字节流（包含 restarts 数组和数量）
    // 调用后不能再 Add，除非先 Reset
    Slice Finish();

    // 返回当前 Block 已使用的字节数（估算，用于判断是否超过 block_size）
    size_t CurrentSizeEstimate() const;

    // 是否为空（还没有 Add 任何 kv）
    bool empty() const { return buffer_.empty(); }

private:
    int                   block_restart_interval_; // 间隔n个key设置restart point
    std::string           buffer_;                 // 已编码的entry字节流
    std::vector<uint32_t> restarts_;               // 每个 restart point 的 buffer_ 偏移
    int                   counter_  = 0;           // 距上次 restart 已经写了几个 key
    bool                  finished_ = false;       // 是否已调用 Finish()
    std::string           last_key_;               // 上一个写入的 key（用于计算共享前缀）
};
} // namespace mapledb