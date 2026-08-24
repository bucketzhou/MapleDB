/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-23 18:12:07
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-24 10:59:35
 * @Description: Arena：内存池分配器，专为大量小对象、统一生命周期的场景设计,设计参考
 * LevelDB::Arena，SkipList 的所有节点都从这里分配
 */
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mapledb {
class Arena {
public:
    Arena() = default;
    // 禁止拷贝语义，应独占内存
    Arena(const Arena&)      = delete;
    Arena& operator=(Arena&) = delete;
    ~Arena();

    /* --------------------------------- 核心分配接口 --------------------------------- */
    // 分配 bytes 字节，返回指针（不保证对齐）
    // 注意：不支持释放单个对象，只能整体销毁 Arena
    char* Allocate(size_t bytes);

    // 分配 bytes 字节，保证按平台指针大小对齐
    // SkipList 节点里存放指针数组时必须用这个，否则可能导致 UB
    char* AllocateAligned(size_t bytes);

    // 返回 Arena 当前累计分配的内存量（含内部碎片）
    // 用于判断 MemTable 是否达到 write_buffer_size
    size_t MemoryUsage() const {
        // memory_order_relaxed：只需要原子读，不需要同步其他内存操作
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    /* ---------------------------------- 内部实现 ---------------------------------- */

    // Allocate 的慢速路径：当前 Block 空间不足时调用
    char* AllocateFallback(size_t bytes);

    // 向操作系统申请一个新的 Block（block_bytes 字节）
    char* AllocateNewBlock(size_t block_bytes);
    // 当前 Block 中下一次分配的起始位置
    char* alloc_ptr_ = nullptr;
    // 当前 Block 中剩余的可用字节数
    size_t alloc_bytes_remaining_ = 0;
    // 所有已申请的 Block 列表（析构时统一释放）
    std::vector<char*> blocks_;

    // 累计内存用量（用 atomic 保证读取时线程安全）
    std::atomic<size_t> memory_usage_ = 0;
    // 每个 Block 的默认大小：4096 字节（与 OS 内存页大小对齐）
    static constexpr size_t kBlockSize = 4096;
};

inline char* Arena::Allocate(size_t bytes) {
    assert(bytes > 0);

    // 快速路径：当前 Block 空间足够，直接"指针碰撞"分配
    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    // 慢速路径：当前 Block 不够用，需要申请新 Block
    return AllocateFallback(bytes);
}

} // namespace mapledb