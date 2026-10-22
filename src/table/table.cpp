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
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace mapledb {

// ── CRC32（与 Day6 table_builder.cpp 保持一致）─────────────────

static uint32_t tbl_crc_table[256];
static bool     tbl_crc_init = false;

static void TblInitCRC() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        tbl_crc_table[i] = c;
    }
    tbl_crc_init = true;
}

// 计算 CRC32（目前仅用于可选的校验，避免 unused 警告）
[[maybe_unused]]
static uint32_t TblCRC32(const char* data, size_t len) {
    if (!tbl_crc_init)
        TblInitCRC();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = tbl_crc_table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
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
    size_t      block_data_size = static_cast<size_t>(ih.size());

    char* buf = new char[block_data_size];
    fseek(f, static_cast<long>(ih.offset()), SEEK_SET);

    if (fread(buf, 1, block_data_size, f) != block_data_size) {
        delete[] buf;
        fclose(f);
        return Status::IOError("cannot read index block");
    }

    // 跳过 Index Block 的 5 字节物理尾部（type + CRC）
    char trailer[5];
    if (fread(trailer, 1, 5, f) != 5) {
        delete[] buf;
        fclose(f);
        return Status::IOError("cannot read index block trailer");
    }
    // trailer[0] = compression type（0 = kNoCompression）
    // trailer[1..4] = CRC32（教学版跳过验证）

    index_block = new Block(buf, block_data_size, true /* owned，析构时 delete[] */);

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
    *table        = new Table(rep);
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
// 从文件中读取一个 Block（public，供 TwoLevelIterator 使用）

// Status Table::ReadBlock(const BlockHandle& handle, Block** result) const {
//     *result = nullptr;

//     size_t n   = static_cast<size_t>(handle.size());
//     char*  buf = new char[n];

//     // 定位到 Block 起始位置并读取 n 字节
//     fseek(rep_->file, static_cast<long>(handle.offset()), SEEK_SET);
//     if (fread(buf, 1, n, rep_->file) != n) {
//         delete[] buf;
//         return Status::IOError("cannot read data block");
//     }

//     // 读取并跳过 5 字节物理尾部（type=1B + CRC=4B）
//     char trailer[5];
//     if (fread(trailer, 1, 5, rep_->file) != 5) {
//         delete[] buf;
//         return Status::IOError("cannot read data block trailer");
//     }
//     // 教学版：跳过 CRC 校验，生产代码应在此验证

//     *result = new Block(buf, n, true /* owned */);
//     return Status::OK();
// }

// ── Table::ReadBlock ──────────────────────────────────────────
// 从文件中读取一个 Block（public，供 TwoLevelIterator 使用）
//
// Day11 新增：若 options_.block_cache != nullptr，先查缓存。
//   缓存 key   = [cache_id: 8B][block_offset: 8B]
//   缓存 value = char*（Block 原始字节，由缓存 delete[]）
//   命中时：从缓存字节构造新 Block（Block 自己 new 一份，拥有所有权）
//   未命中时：从磁盘读取，同时将原始字节的副本插入缓存

Status Table::ReadBlock(const BlockHandle& handle, Block** result) const {
    *result = nullptr;

    // ── Day11: 尝试 BlockCache 命中 ───────────────────────
    Cache* bc = rep_->options.block_cache;
    if (bc != nullptr && rep_->cache_id != 0) {
        // 构造 16 字节缓存 key：[cache_id(8B)][offset(8B)]
        char key_buf[16];
        EncodeFixed64(key_buf, rep_->cache_id);
        EncodeFixed64(key_buf + 8, handle.offset());
        Slice cache_key(key_buf, 16);

        Cache::Handle* ch = bc->Lookup(cache_key);
        if (ch != nullptr) {
            // 缓存命中：复制缓存中的原始字节，构造新 Block
            const char* cached = static_cast<const char*>(bc->Value(ch));
            size_t      n      = static_cast<size_t>(handle.size());
            char*       buf    = new char[n];
            memcpy(buf, cached, n);
            bc->Release(ch);
            *result = new Block(buf, n, true /* owned */);
            return Status::OK();
        }
    }

    // ── 磁盘读取 ──────────────────────────────────────────
    size_t n   = static_cast<size_t>(handle.size());
    char*  buf = new char[n];

    // 修复：同一个 Table 会被并发 Get 共享，fseek/fread 组合会互相改写文件位置。
    // pread 使用独立偏移，且短读/信号中断时继续读取剩余字节。
    auto read_at = [this](char* dst, size_t size, uint64_t offset) {
        size_t done = 0;
        while (done < size) {
            ssize_t count = pread(fileno(rep_->file), dst + done, size - done,
                                  static_cast<off_t>(offset + done));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            done += static_cast<size_t>(count);
        }
        return true;
    };
    if (!read_at(buf, n, handle.offset())) {
        delete[] buf;
        return Status::IOError("cannot read data block");
    }

    // 读取并跳过 5 字节物理尾部（type=1B + CRC=4B）
    char trailer[5];
    if (!read_at(trailer, sizeof(trailer), handle.offset() + n)) {
        delete[] buf;
        return Status::IOError("cannot read data block trailer");
    }
    // 教学版：跳过 CRC 校验，生产代码应在此验证

    // ── Day11: 将原始字节插入 BlockCache ──────────────────
    if (bc != nullptr && rep_->cache_id != 0) {
        char key_buf[16];
        EncodeFixed64(key_buf, rep_->cache_id);
        EncodeFixed64(key_buf + 8, handle.offset());
        Slice cache_key(key_buf, 16);

        // 缓存存储独立副本，由缓存 delete[]
        char* cached_copy = new char[n];
        memcpy(cached_copy, buf, n);
        Cache::Handle* ch = bc->Insert(cache_key, cached_copy, n, [](const Slice&, void* v) {
            delete[] static_cast<char*>(v);
        });
        bc->Release(ch); // 释放 Insert 返回的引用
    }

    *result = new Block(buf, n, true /* owned */);
    return Status::OK();
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
            s                 = ReadBlock(handle, &data_block);

            if (s.Ok()) {
                // ④ 在 DataBlock 里 Seek(key)
                Block::Iter* diter = data_block->NewIterator(rep_->cmp);
                diter->Seek(key);

                if (diter->Valid()) {
                    (*handle_result)(arg, diter->key(), diter->value());
                }
                s = diter->status();
                delete diter;
                delete data_block;
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
    TwoLevelIterator(Block::Iter* index_iter, Table* table, const Comparator* cmp) :
        index_iter_(index_iter), data_iter_(nullptr), owned_block_(nullptr), table_(table), cmp_(cmp) {}

    ~TwoLevelIterator() override {
        delete data_iter_;
        delete owned_block_;
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
        delete owned_block_;
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
        s            = table_->ReadBlock(handle, &block);
        if (!s.Ok()) {
            status_ = s;
            return;
        }

        owned_block_ = block;
        // 修复：同键版本按 sequence 降序存储，不能用原始字节序做 Seek。
        data_iter_   = block->NewIterator(cmp_);
    }

    // 跳过已遍历完的空 DataBlock，推进到下一个有 entry 的位置
    void SkipEmptyDataBlocksForward() {
        while (data_iter_ == nullptr || !data_iter_->Valid()) {
            if (!index_iter_->Valid()) {
                // Index Block 也遍历完了，停止
                delete data_iter_;
                delete owned_block_;
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
    Block*       owned_block_; // 当前 data_iter_ 对应的 Block（负责生命周期）
    Table*       table_;       // 指向所属 Table，用于读取 DataBlock
    const Comparator* cmp_;   // 与写入文件及 Index Block 使用相同的排序规则。
    Status       status_;      // 错误状态
};

// ── Table::NewIterator ────────────────────────────────────────

Table::Iterator* Table::NewIterator() {
    // 修复：索引定位与数据块定位必须使用 Table::Open 传入的同一比较器。
    Block::Iter* index_iter = rep_->index_block->NewIterator(rep_->cmp);
    return new TwoLevelIterator(index_iter, this, rep_->cmp);
}

} // namespace mapledb