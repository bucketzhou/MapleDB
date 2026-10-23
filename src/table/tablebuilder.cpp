/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-11 16:57:53
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-11 17:02:04
 * @Description:
 */

#include "table/tablebuilder.h"

#include "table/blockbuilder.h"
#include "util/bloom.h"
#include "util/coding.h"
#include "util/crc32.h"

#include <cassert>
#include <cstring>
#include <unistd.h>

namespace mapledb {


// ── BlockBuilderWrapper：包装 BlockBuilder，让 .h 不暴露实现 ──

struct TableBuilder::BlockBuilderWrapper {
    BlockBuilder builder;
    explicit BlockBuilderWrapper(int restart_interval) : builder(restart_interval) {}
};

// ── 构造/析构 ─────────────────────────────────────────────────

TableBuilder::TableBuilder(const std::string& filename, const Options& options, const Comparator* cmp) :
    options_(options), cmp_(cmp), file_(nullptr), offset_(0), num_entries_(0), closed_(false),
    pending_index_entry_(false) {
    file_ = fopen(filename.c_str(), "wb");
    if (!file_) {
        status_ = Status::IOError("cannot create SSTable file: " + filename);
        return;
    }

    // 创建 data block 和 index block 构建器
    // index block 的 restart_interval=1（每条 entry 都是 restart point，
    // 方便二分查找）
    data_block_  = new BlockBuilderWrapper(options_.block_restart_interval);
    index_block_ = new BlockBuilderWrapper(1);
}

TableBuilder::~TableBuilder() {
    if (file_ && !closed_) {
        // 析构时如果还没关闭，丢弃（不写 Footer，文件不完整）
        Abandon();
    }
    delete data_block_;
    delete index_block_;
}

// ── ComputeCRC ────────────────────────────────────────────────

uint32_t TableBuilder::ComputeCRC(const char* data, size_t n) const { return crc32::Extend(data, n); }

// ── WriteBlock：把一段字节流写入文件（追加 type+CRC 尾部）────────

void TableBuilder::WriteBlock(const Slice& block_contents, BlockHandle* handle) {
    if (!status_.Ok())
        return;

    // 记录这个 Block 的起始偏移
    handle->set_offset(offset_);
    handle->set_size(block_contents.Size());

    // 写内容
    if (fwrite(block_contents.Data(), 1, block_contents.Size(), file_) != block_contents.Size()) {
        status_ = Status::IOError("SSTable write data block failed");
        return;
    }

    // 追加 1 字节 compression type（0 = kNoCompression）
    char trailer[5];
    trailer[0] = 0; // kNoCompression

    // 修改：正文只校验一次，再对 type 增量计算；不复制整块，也不改变磁盘校验格式。
    uint32_t crc = crc32::Extend(block_contents.Data(), block_contents.Size());
    crc = crc32::Extend(trailer, 1, crc);
    trailer[1] = static_cast<char>(crc & 0xFF);
    trailer[2] = static_cast<char>((crc >> 8) & 0xFF);
    trailer[3] = static_cast<char>((crc >> 16) & 0xFF);
    trailer[4] = static_cast<char>((crc >> 24) & 0xFF);

    if (fwrite(trailer, 1, 5, file_) != 5) {
        status_ = Status::IOError("SSTable write block trailer failed");
        return;
    }

    // 更新文件偏移（block 内容 + 5 字节尾部）
    offset_ += block_contents.Size() + 5;
}

// ── Flush：把当前 data_block_ 写入文件，更新 index 状态 ─────────

void TableBuilder::Flush() {
    if (!status_.Ok())
        return;
    if (data_block_->builder.empty())
        return;

    assert(!pending_index_entry_);

    // 完成 data block 构建，写入文件
    WriteBlock(data_block_->builder.Finish(), &pending_handle_);

    data_block_->builder.Reset();

    // 标记：下一次 Add 时需要往 index_block_ 写这个 block 的 entry
    // （延迟写 index entry 是为了用"最小分隔 key"而不是当前 block 的最后一个 key，
    //  等看到下一个 block 的第一个 key 之后，才能确定分隔 key）
    pending_index_entry_ = true;

    // 刷新文件缓冲（不一定 fsync，但保证数据在内核缓冲区）
    // 修复：缓冲写入的磁盘错误可能延迟到 fflush 才出现，不能丢掉错误。
    if (fflush(file_) != 0 && status_.Ok())
        status_ = Status::IOError("SSTable flush failed");
}

// ── Add：追加一条 kv ──────────────────────────────────────────

void TableBuilder::Add(const Slice& key, const Slice& value) {
    if (!status_.Ok())
        return;
    assert(!closed_);

    if (options_.bloom_bits_per_key > 0) {
        // 修复：内部键要先验证再去掉 tag，且 tombstone 也必须加入，不能漏掉删除记录。
        if (!cmp_->IsValidKey(key)) {
            status_ = Status::Corruption("invalid key for SSTable filter");
            return;
        }
        filter_keys_.push_back(cmp_->FilterKey(key).ToString());
    }

    // 如果上一个 DataBlock 刚 flush 完，先写 index entry
    // index entry 的 key 用 last_key_（上一个 block 的最后一个 key）
    // 这里简化：直接用 last_key_ 作为分隔 key
    // （LevelDB 原版会用 FindShortestSeparator 缩短 key，节省 Index Block 空间）
    if (pending_index_entry_) {
        std::string handle_encoding;
        pending_handle_.EncodeTo(&handle_encoding);
        index_block_->builder.Add(Slice(last_key_), Slice(handle_encoding));
        pending_index_entry_ = false;
    }

    // 记录 last_key_
    last_key_.assign(key.Data(), key.Size());
    num_entries_++;

    // 追加到当前 data block
    data_block_->builder.Add(key, value);

    // 如果 data block 超过 block_size，flush
    if (data_block_->builder.CurrentSizeEstimate() >= options_.block_size) {
        Flush();
    }
}

// ── Finish：完成整个文件写入 ──────────────────────────────────

Status TableBuilder::Finish() {
    assert(!closed_);
    // 完成关闭后才标记 closed；中途失败仍由析构函数关闭文件，避免泄漏句柄。

    // 1. Flush 最后一个（可能不满）的 data block
    Flush();
    if (!status_.Ok())
        return status_;

    // 如果最后一个 data block 非空，补写 index entry
    if (pending_index_entry_) {
        std::string handle_encoding;
        pending_handle_.EncodeTo(&handle_encoding);
        index_block_->builder.Add(Slice(last_key_), Slice(handle_encoding));
        pending_index_entry_ = false;
    }

    // 2. 每张 SSTable 写一个过滤器；通过 MetaIndex 保存位置，旧文件的空 MetaIndex 仍有效。
    BlockBuilder meta_index_builder(1);
    if (options_.bloom_bits_per_key > 0) {
        std::vector<Slice> keys;
        keys.reserve(filter_keys_.size());
        for (const auto& key : filter_keys_)
            keys.emplace_back(key);
        std::string filter;
        BloomFilter(options_.bloom_bits_per_key).CreateFilter(keys, &filter);
        BlockHandle filter_handle;
        WriteBlock(Slice(filter), &filter_handle); // 同样写入 type + CRC，防止损坏导致假阴性。
        std::string encoded;
        filter_handle.EncodeTo(&encoded);
        meta_index_builder.Add(Slice(cmp_->FilterName()), Slice(encoded));
    }
    BlockHandle metaindex_handle;
    WriteBlock(meta_index_builder.Finish(), &metaindex_handle);
    if (!status_.Ok())
        return status_;

    // 3. 写 Index Block
    BlockHandle index_handle;
    WriteBlock(index_block_->builder.Finish(), &index_handle);
    if (!status_.Ok())
        return status_;

    // 4. 写 Footer（48 字节）
    Footer footer;
    footer.set_metaindex_handle(metaindex_handle);
    footer.set_index_handle(index_handle);

    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);

    assert(footer_encoding.size() == Footer::kEncodedLength);

    if (fwrite(footer_encoding.data(), 1, footer_encoding.size(), file_)
        != footer_encoding.size()) {
        status_ = Status::IOError("SSTable write footer failed");
        return status_;
    }
    offset_ += footer_encoding.size();

    // 修复：尾部刷写或关闭失败时，禁止把这个 SSTable 当作完整文件安装。
    if (fflush(file_) != 0)
        status_ = Status::IOError("SSTable flush failed");
    // 修复：SSTable 必须先可靠落盘，之后才能在 MANIFEST 中替代旧 WAL/输入文件。
    if (status_.Ok() && fsync(fileno(file_)) != 0)
        status_ = Status::IOError("SSTable sync failed");
    if (fclose(file_) != 0 && status_.Ok())
        status_ = Status::IOError("SSTable close failed");
    file_ = nullptr;
    closed_ = true;

    return status_;
}

void TableBuilder::Abandon() {
    assert(!closed_);
    closed_ = true;
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

} // namespace mapledb
