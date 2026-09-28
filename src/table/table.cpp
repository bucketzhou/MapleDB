/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-12 13:29:33
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-21 10:55:55
 * @Description:
 *Table 的实现：Open / ReadBlock / InternalGet / NewIterator
 *修复说明：1. #include "table/block.h" 已包含完整的 Block::Iter 定义，消除"不完整类型"错误。
 *2. ReadBlock 改为 public，TwoLevelIterator 可直接调用。
 */
#include "table/table.h"

#include "mapledb/options.h"
#include "util/cache.h"
#include "util/coding.h"

#include <cassert>
#include <cstring>
#include <unistd.h>
#include <memory>
#include <limits>
#include <cerrno>
#include <mutex>

namespace mapledb {

// ── CRC32（与 Day6 table_builder.cpp 保持一致）─────────────────

static uint32_t tbl_crc_table[256];
static std::once_flag tbl_crc_init;

static void TblInitCRC() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        tbl_crc_table[i] = c;
    }
}

// CRC 初始化由 call_once 保护，多个读线程不会观察到部分初始化结果。
[[maybe_unused]]
static uint32_t TblCRC32(const char* data, size_t len) {
    std::call_once(tbl_crc_init, TblInitCRC);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = tbl_crc_table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static Status ReadRawBlock(FILE* file, uint64_t file_size, const BlockHandle& handle,
                           std::unique_ptr<char[]>* result, size_t* size, bool verify_crc) {
    // 用减法检查边界，避免 offset + size 溢出后错误接受损坏的 BlockHandle。
    if (handle.offset() > file_size || file_size - handle.offset() < 5 ||
        handle.size() > file_size - handle.offset() - 5 ||
        handle.size() > std::numeric_limits<size_t>::max() - 5)
        return Status::Corruption("invalid block bounds");
    const size_t n = static_cast<size_t>(handle.size());
    std::unique_ptr<char[]> buf(new char[n + 5]);
    size_t done = 0;
    while (done < n + 5) {
        // 并发 Get 共享 Table；pread 不移动 FILE 游标，避免 fseek/fread 相互干扰。
        // 短读继续补齐，EINTR 重试，EOF/错误不能当作完整块交给解码器。
        const ssize_t count = pread(fileno(file), buf.get() + done, n + 5 - done,
                                   static_cast<off_t>(handle.offset() + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Status::IOError("cannot read block");
        done += static_cast<size_t>(count);
    }
    if (buf[n] != 0) return Status::Corruption("unsupported block compression");
    if (verify_crc && TblCRC32(buf.get(), n + 1) != DecodeFixed32(buf.get() + n + 1))
        return Status::Corruption("block checksum mismatch");
    *size = n;
    *result = std::move(buf);
    return Status::OK();
}

// ── Table::Open ──────────────────────────────────────────────

Status Table::Open(const std::string& filename, const Options& options, const Comparator* cmp,
                   Table** table) {
    *table = nullptr;

    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) {
        return Status::IOError("cannot open SSTable: " + filename);
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    uint64_t file_size = static_cast<uint64_t>(ftell(f));

    if (file_size < Footer::kEncodedLength) {
        fclose(f);
        return Status::Corruption("SSTable file too small");
    }

    // ① 读 Footer（末尾固定 48 字节）
    char footer_buf[Footer::kEncodedLength];
    fseek(f, -static_cast<long>(Footer::kEncodedLength), SEEK_END);
    if (fread(footer_buf, 1, Footer::kEncodedLength, f) != Footer::kEncodedLength) {
        fclose(f);
        return Status::IOError("cannot read SSTable footer");
    }

    Footer footer;
    Status s = footer.DecodeFrom(Slice(footer_buf, Footer::kEncodedLength));
    if (!s.Ok()) {
        fclose(f);
        return s;
    }

    // ② 根据 index_handle 读取 Index Block，常驻内存
    Block*      index_block     = nullptr;
    BlockHandle ih              = footer.index_handle();
    size_t block_data_size = 0;
    std::unique_ptr<char[]> buf;
    s = ReadRawBlock(f, file_size, ih, &buf, &block_data_size, false);
    if (!s.Ok()) {
        fclose(f);
        return s;
    }
    index_block = new Block(buf.release(), block_data_size, true);

    // ③ 构建 Rep 并返回 Table 对象
    Rep* rep              = new Rep;
    rep->file             = f;
    rep->file_size        = file_size;
    rep->index_block      = index_block;
    rep->options          = options;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->cmp              = cmp; // ← 保存比较器

    // Day11: 若启用了 block_cache，申请一个唯一 ID 用于区分不同文件的 Block
    rep->cache_id = (options.block_cache != nullptr) ? options.block_cache->NewId() : 0;
    std::unique_ptr<Table> opened(new Table(rep));
    *table = opened.release();
    return Status::OK();
}

// ── Table::~Table ─────────────────────────────────────────────

Table::~Table() {
    delete rep_->index_block;
    if (rep_->file)
        fclose(rep_->file);
    delete rep_;
}

// ── Table::ReadBlock ──────────────────────────────────────────

Status Table::ReadBlock(const BlockHandle& handle, Block** result,
                        Cache::Handle** cache_handle) const {
    *result = nullptr;
    if (cache_handle) *cache_handle = nullptr;
    Cache* bc = cache_handle ? rep_->options.block_cache : nullptr;
    char key_buf[16];
    EncodeFixed64(key_buf, rep_->cache_id);
    EncodeFixed64(key_buf + 8, handle.offset());
    Slice cache_key(key_buf, sizeof(key_buf));
    if (bc) {
        *cache_handle = bc->Lookup(cache_key);
        if (*cache_handle) {
            // 复用 Block，保留 handle 到块迭代器销毁；缓存淘汰不会提前释放该块。
            *result = static_cast<Block*>(bc->Value(*cache_handle));
            return Status::OK();
        }
    }
    size_t n = 0;
    std::unique_ptr<char[]> buf;
    Status s = ReadRawBlock(rep_->file, rep_->file_size, handle, &buf, &n, false);
    if (!s.Ok()) return s;
    Block* block = new Block(buf.release(), n, true);
    if (bc) {
        *cache_handle = bc->Insert(cache_key, block, n, [](const Slice&, void* v) {
            delete static_cast<Block*>(v);
        });
    }
    *result = block;
    return Status::OK();
}

void Table::ReleaseBlock(Block* block, Cache::Handle* cache_handle) const {
    // 有 handle 时缓存拥有 Block；否则调用者拥有。两种所有权只能释放一次。
    if (cache_handle) rep_->options.block_cache->Release(cache_handle);
    else delete block;
}

// ── Table::InternalGet ────────────────────────────────────────

Status Table::InternalGet(const Slice& key, void* arg,
                          void (*handle_result)(void*, const Slice&, const Slice&)) {
    Status s;

    // ① 在 Index Block 里 Seek(key)，找目标 DataBlock 的 handle
    Block::Iter* iiter = rep_->index_block->NewIterator(rep_->cmp);
    iiter->Seek(key);

    if (iiter->Valid()) {
        // ② 解码 DataBlock 的 BlockHandle
        Slice       handle_value = iiter->value();
        BlockHandle handle;
        Slice       tmp = handle_value;
        s               = handle.DecodeFrom(&tmp);

        if (s.Ok()) {
            // ③ 从文件中读取目标 DataBlock
            Block* data_block = nullptr;
            Cache::Handle* cache_handle = nullptr;
            s = ReadBlock(handle, &data_block, &cache_handle);

            if (s.Ok()) {
                // ④ 在 DataBlock 里 Seek(key)
                Block::Iter* diter = data_block->NewIterator(rep_->cmp);
                diter->Seek(key);

                if (diter->Valid()) {
                    (*handle_result)(arg, diter->key(), diter->value());
                }
                s = diter->status();
                delete diter;
                ReleaseBlock(data_block, cache_handle);
            }
        }
    }

    if (s.Ok())
        s = iiter->status();
    delete iiter;
    return s;
}

// ════════════════════════════════════════════════════════
// TwoLevelIterator：两级迭代器（Index Block → DataBlock）
// 外层遍历 Index Block，内层遍历每个 DataBlock
// ════════════════════════════════════════════════════════

class TwoLevelIterator : public Table::Iterator {
public:
    TwoLevelIterator(Block::Iter* index_iter, Table* table) :
        index_iter_(index_iter), data_iter_(nullptr), cache_handle_(nullptr), owned_block_(nullptr), table_(table) {}

    ~TwoLevelIterator() override {
        delete data_iter_;
        table_->ReleaseBlock(owned_block_, cache_handle_);
        cache_handle_ = nullptr;
        delete index_iter_;
    }

    // ── 移动操作 ──────────────────────────────────────────────

    void SeekToFirst() override {
        index_iter_->SeekToFirst();
        InitDataBlock();
        if (data_iter_)
            data_iter_->SeekToFirst();
        SkipEmptyDataBlocksForward();
    }

    void Seek(const Slice& target) override {
        index_iter_->Seek(target);
        InitDataBlock();
        if (data_iter_)
            data_iter_->Seek(target);
        SkipEmptyDataBlocksForward();
    }

    void Next() override {
        assert(Valid());
        data_iter_->Next();
        SkipEmptyDataBlocksForward();
    }

    // ── 状态查询 ──────────────────────────────────────────────

    bool Valid() const override { return (data_iter_ != nullptr) && data_iter_->Valid(); }

    const Slice& key() const override {
        assert(Valid());
        return data_iter_->key();
    }

    const Slice& value() const override {
        assert(Valid());
        return data_iter_->value();
    }

    Status status() const override {
        if (!index_iter_->status().Ok())
            return index_iter_->status();
        if (data_iter_ && !data_iter_->status().Ok())
            return data_iter_->status();
        return status_;
    }

private:
    // 根据 index_iter_ 当前位置加载对应的 DataBlock
    void InitDataBlock() {
        // 释放旧 DataBlock
        delete data_iter_;
        table_->ReleaseBlock(owned_block_, cache_handle_);
        cache_handle_ = nullptr;
        data_iter_   = nullptr;
        owned_block_ = nullptr;

        if (!index_iter_->Valid())
            return;

        // 从 index entry 的 value 解码 BlockHandle
        Slice       handle_value = index_iter_->value();
        BlockHandle handle;
        Slice       tmp = handle_value;
        Status      s   = handle.DecodeFrom(&tmp);
        if (!s.Ok()) {
            status_ = s;
            return;
        }

        // 读取 DataBlock（table_ 是 public ReadBlock）
        Block* block = nullptr;
        s = table_->ReadBlock(handle, &block, &cache_handle_);
        if (!s.Ok()) {
            status_ = s;
            return;
        }

        owned_block_ = block;
        data_iter_   = block->NewIterator(table_->ComparatorForIterator());
    }

    // 跳过已遍历完的空 DataBlock，推进到下一个有 entry 的位置
    void SkipEmptyDataBlocksForward() {
        while (data_iter_ == nullptr || !data_iter_->Valid()) {
            if (!status().Ok()) return;
            if (!index_iter_->Valid()) {
                // Index Block 也遍历完了，停止
                delete data_iter_;
                table_->ReleaseBlock(owned_block_, cache_handle_);
                cache_handle_ = nullptr;
                data_iter_   = nullptr;
                owned_block_ = nullptr;
                return;
            }
            index_iter_->Next();
            InitDataBlock();
            if (data_iter_)
                data_iter_->SeekToFirst();
        }
    }

    Block::Iter* index_iter_;  // Index Block 的迭代器
    Block::Iter* data_iter_;   // 当前 DataBlock 的迭代器（可为 nullptr）
    Cache::Handle* cache_handle_;
    Block*       owned_block_; // 当前 data_iter_ 对应的 Block（负责生命周期）
    Table*       table_;       // 指向所属 Table，用于读取 DataBlock
    Status       status_;      // 错误状态
};

// ── Table::NewIterator ────────────────────────────────────────

Table::Iterator* Table::NewIterator() {
    Block::Iter* index_iter = rep_->index_block->NewIterator(rep_->cmp);
    return new TwoLevelIterator(index_iter, this);
}

} // namespace mapledb
