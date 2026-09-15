/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-12 13:26:42
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-12 13:27:00
 * @Description: src/table/block.cpp Block 的构造/析构和 NewIterator 实现
 * 注意：Block::Iter 的完整定义已移至 block.h
 */

#include "table/block.h"

#include <cassert>

namespace mapledb {

// ── Block 构造函数 ─────────────────────────────────────────────

Block::Block(const char* data, size_t size, bool owned) : data_(data), size_(size), owned_(owned) {
    // Block 末尾 4 字节是 num_restarts（fixed32 小端序）
    if (size_ < sizeof(uint32_t)) {
        // 数据太小，设为空 Block
        num_restarts_   = 0;
        restart_offset_ = 0;
        return;
    }

    // 读取 num_restarts
    num_restarts_ = DecodeFixed32(data_ + size_ - sizeof(uint32_t));

    // 计算 restarts 数组占用字节数
    //   restarts 数组：num_restarts * 4 字节
    //   num_restarts 字段：4 字节
    //   合计：(num_restarts + 1) * 4 字节
    size_t restarts_size = (static_cast<size_t>(num_restarts_) + 1) * sizeof(uint32_t);

    if (restarts_size > size_) {
        // num_restarts 值非法（超出 Block 大小），视为损坏
        num_restarts_   = 0;
        restart_offset_ = 0;
        return;
    }

    // restarts 数组起始偏移 = size_ - (num_restarts+1)*4
    restart_offset_ = static_cast<uint32_t>(size_ - restarts_size);
}

// ── Block 析构函数 ─────────────────────────────────────────────

Block::~Block() {
    if (owned_) {
        delete[] data_;
    }
}

// ── Block::NewIterator ────────────────────────────────────────

Block::Iter* Block::NewIterator(const Comparator* cmp) const {
    // 返回一个新的迭代器，初始状态为无效（调用方负责 delete）
    return new Iter(cmp, data_, restart_offset_, num_restarts_);
}

} // namespace mapledb