/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-11 13:15:53
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-11 16:26:18
 * @Description: SSTable 文件格式相关：BlockHandle、Footer
 */
#pragma once

#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "util/coding.h"

#include <cstdint>
#include <string>

namespace mapledb {
// ════════════════════════════════════════════════════════
// BlockHandle：指向文件中某个 Block 的位置指针
// 包含 offset（起始字节偏移）和 size（字节数，不含 5 字节尾部）
// ════════════════════════════════════════════════════════
class BlockHandle {
public:
    // varint64 最大 10 字节，两个 varint64 最大 20 字节
    static constexpr size_t kMaxEncodedLength = 10 + 10;

    BlockHandle() : offset_(0), size_(0) {}
    BlockHandle(uint64_t offset, uint64_t size) : offset_(offset), size_(size) {}

    uint64_t offset() const { return offset_; }
    uint64_t size() const { return size_; }
    void     set_offset(uint64_t o) { offset_ = o; }
    void     set_size(uint64_t s) { size_ = s; }

    bool ValidWithin(uint64_t limit) const {
        // 修复：先做减法校验，避免 offset + size + trailer 的无符号加法溢出。
        return offset_ <= limit && size_ <= UINT32_MAX && size_ <= limit - offset_
               && limit - offset_ - size_ >= 5;
    }

    // 编码为 varint64+varint64，追加到 dst
    void EncodeTo(std::string* dst) const {
        PutVarint64(dst, offset_);
        PutVarint64(dst, size_);
    }

    // 从 input 解码，消耗 input 的前缀
    Status DecodeFrom(Slice* input) {
        if (!GetVarint64(input, &offset_) || !GetVarint64(input, &size_)) {
            return Status::Corruption("bad block handle");
        }
        return Status::OK();
    }

private:
    uint64_t offset_;
    uint64_t size_;
};

// ════════════════════════════════════════════════════════
// Footer：固定 48 字节，位于 SSTable 文件末尾
//
// 布局：
//   metaindex_handle: varint 编码（最多 20 字节）
//   index_handle    : varint 编码（最多 20 字节）
//   padding         : 0 字节填充至 40 字节
//   magic           : uint64 小端序（8 字节）
// 合计 = 48 字节
// ════════════════════════════════════════════════════════
class Footer {
public:
    // LevelDB 的魔数，用于验证文件完整性
    static constexpr uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

    // Footer 固定 48 字节
    static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

    Footer() = default;

    const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
    const BlockHandle& index_handle() const { return index_handle_; }
    void               set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }
    void               set_index_handle(const BlockHandle& h) { index_handle_ = h; }

    // 编码为 48 字节，追加到 dst
    void EncodeTo(std::string* dst) const {
        const size_t original_size = dst->size();

        metaindex_handle_.EncodeTo(dst);
        index_handle_.EncodeTo(dst);

        // 填充至 2 * kMaxEncodedLength = 40 字节
        dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength, 0);

        // 追加魔数（固定 8 字节小端序）
        PutFixed64(dst, kTableMagicNumber);

        assert(dst->size() == original_size + kEncodedLength);
    }

    // 从 48 字节数据解码
    Status DecodeFrom(const Slice& input) {
        if (input.Size() < kEncodedLength) {
            return Status::Corruption("footer too short");
        }

        // 验证魔数（最后 8 字节）
        const char* magic_ptr = input.Data() + kEncodedLength - 8;
        uint64_t    magic     = DecodeFixed64(magic_ptr);
        if (magic != kTableMagicNumber) {
            return Status::Corruption("bad magic number in SSTable footer");
        }

        // 解码两个 BlockHandle
        Slice  s(input.Data(), kEncodedLength - 8);
        Status result = metaindex_handle_.DecodeFrom(&s);
        if (result.Ok()) {
            result = index_handle_.DecodeFrom(&s);
        }
        return result;
    }

private:
    BlockHandle metaindex_handle_;
    BlockHandle index_handle_;
};

} // namespace mapledb
