/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-24 10:54:38
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-24 17:00:38
 * @Description: Arena 的慢速路径和析构实现
 */

#include "arena.h"
// for operator new / delete
#include <cstdlib>
namespace mapledb {
Arena::~Arena() {
    // 统一释放所有 Block，这是 Arena 最大的优势之一：
    // MemTable 销毁时不需要逐个 delete SkipList 节点，O(block数量) 即可
    for (char* block : blocks_) {
        delete[] block;
    }
}
char* Arena::AllocateFallback(size_t bytes) {
    // 策略：大对象（> kBlockSize/4）单独给它一个定制大小的 Block，
    // 不占用通用 Block 的空间，避免浪费当前 Block 剩余的碎片。
    if (bytes > kBlockSize / 4) {
        return AllocateNewBlock(bytes);
    }
    // 小对象：申请一个标准大小的新 Block，从新 Block 里分配
    // 注意：旧 Block 剩余的空间就此"浪费"了（碎片），
    // 但由于 SkipList 节点大多很小，这部分浪费通常可以接受。
    alloc_ptr_             = AllocateNewBlock(kBlockSize);
    alloc_bytes_remaining_ = kBlockSize;
    char* result           = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
    char* result = new char[block_bytes];
    blocks_.push_back(result);
    // 更新内存统计（+sizeof(char*) 是 blocks_ vector 里存指针的开销）
    memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
    return result;
}

char* Arena::AllocateAligned(size_t bytes) {
    // 按指针大小对齐（32位系统4字节，64位系统8字节）
    // SkipList 节点的 next_ 指针数组必须对齐，否则在某些平台会崩溃
    const size_t align = (sizeof(void*) > 8) ? sizeof(void*) : 8;

    // align 必须是2的幂，才能用位运算计算对齐
    static_assert((sizeof(void*) & (sizeof(void*) - 1)) == 0,
                  "Pointer size should be a power of 2");

    // 计算当前指针需要填充多少字节才能对齐
    // 例：align=8，alloc_ptr_=0x...5，则 current_mod=5，slop=3
    size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
    size_t slop        = (current_mod == 0) ? 0 : (align - current_mod);
    size_t needed      = bytes + slop; // 实际需要分配的字节数（含填充）

    char* result;
    if (needed <= alloc_bytes_remaining_) {
        // 快速路径：当前 Block 够用
        result = alloc_ptr_ + slop; // 跳过填充字节，result 已对齐
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
    } else {
        // 慢速路径：AllocateFallback 申请的新 Block 起始地址通常已对齐
        // （new char[] 返回的内存至少按最大基础类型对齐）
        result = AllocateFallback(bytes);
    }

    // 验证对齐
    assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
    return result;
}

} // namespace mapledb