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
#include "util/bloom.h"
#include "util/cache.h"
#include "util/coding.h"
#include "util/crc32.h"

#include <cassert>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

namespace mapledb {

// ── CRC32（与 Day6 table_builder.cpp 保持一致）─────────────────

static uint32_t TblCRC32(const char* data, size_t len, uint32_t previous = 0) {
    return crc32::Extend(data, len, previous);
}

static Status CheckBlock(const char* data, size_t size, const char* trailer) {
    // 修复：格式与写入端一致，校验 contents + type，损坏数据不能交给查询或合并。
    if (trailer[0] != 0)
        return Status::Corruption("unsupported SSTable compression type");
    if (TblCRC32(trailer, 1, TblCRC32(data, size)) != DecodeFixed32(trailer + 1))
        return Status::Corruption("SSTable block checksum mismatch");
    return Status::OK();
}

// 修改：元数据和数据块共用按偏移读取，短读/EINTR 继续，真实错误及提前 EOF 返回失败。
static bool ReadAt(int fd, char* data, size_t size, uint64_t offset, const void* mapping = nullptr) {
    // 调用方先验证范围；元数据从映射区复制到独立缓冲，保留打开时已校验的索引快照。
    if (mapping) {
        memcpy(data, static_cast<const char*>(mapping) + offset, size);
        return true;
    }
    size_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, data + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

static Status ReadMetadata(int fd, const BlockHandle& handle, std::string* contents,
                           const void* mapping) {
    // 调用前已按文件/元数据边界验证 handle；过滤器也必须校验 CRC 后才能用于排除 key。
    if (handle.size() > SIZE_MAX - 5)
        return Status::Corruption("metadata block size overflow");
    contents->resize(static_cast<size_t>(handle.size()) + 5);
    if (!ReadAt(fd, contents->data(), contents->size(), handle.offset(), mapping))
        return Status::IOError("cannot read SSTable filter metadata");
    Status s = CheckBlock(contents->data(), handle.size(), contents->data() + handle.size());
    contents->resize(handle.size());
    return s;
}

static Status ReadFilter(int fd, const BlockHandle& meta_handle, const Comparator* cmp,
                         std::string* filter, const void* mapping) {
    std::string contents;
    Status s = ReadMetadata(fd, meta_handle, &contents, mapping);
    if (!s.Ok())
        return s;
    Block meta(contents.data(), contents.size());
    std::unique_ptr<Block::Iter> it(meta.NewIterator(BytewiseComparator()));
    const Slice name(cmp->FilterName());
    it->Seek(name);
    // 修复：旧文件无该条目或键编码不同，沿用普通查询，不能使用不匹配的过滤器。
    if (!it->Valid() || it->key() != name)
        return it->status();
    Slice encoded = it->value();
    BlockHandle handle;
    s = handle.DecodeFrom(&encoded);
    if (!s.Ok())
        return s;
    if (!encoded.Empty() || !handle.ValidWithin(meta_handle.offset())
        || handle.size() < 2 || handle.size() - 1 > UINT32_MAX / 8)
        return Status::Corruption("invalid SSTable filter handle");
    s = ReadMetadata(fd, handle, filter, mapping);
    if (!s.Ok())
        return s;
    // 拒绝残缺过滤器或非法探测次数，避免把已有 key 误判为不存在。
    if (filter->size() < 2 || static_cast<uint8_t>(filter->back()) == 0
        || static_cast<uint8_t>(filter->back()) > 30)
        return Status::Corruption("invalid SSTable Bloom filter");
    return Status::OK();
}

// ── Table::Open ──────────────────────────────────────────────

Status Table::Open(const std::string& filename, const Options& options, const Comparator* cmp,
                   Table** table, uint64_t cache_id, uint64_t file_number) {
    *table = nullptr;
    // 修改：直接打开 fd；局部守卫覆盖所有错误路径，成功后再转交 Table。
    struct FileGuard {
        int fd;
        ~FileGuard() { if (fd >= 0) close(fd); }
    } file{open(filename.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0)
        return Status::IOError("cannot open SSTable: " + filename);
    struct stat info;
    if (fstat(file.fd, &info) != 0 || info.st_size < 0)
        return Status::IOError("cannot stat SSTable: " + filename);
    const uint64_t file_size = static_cast<uint64_t>(info.st_size);
    if (file_size < Footer::kEncodedLength)
        return Status::Corruption("SSTable file too small");

    std::shared_ptr<const void> mapping;
    if (options.use_mmap_reads && sizeof(void*) >= 8 && file_size <= SIZE_MAX) {
        void* base = mmap(nullptr, static_cast<size_t>(file_size), PROT_READ, MAP_SHARED, file.fd, 0);
        if (base != MAP_FAILED) {
            // 修改：映射由最后一个 Table/Block 所有者释放，不能仅随 Table 析构解除。
            mapping = {base, [file_size](const void* p) {
                munmap(const_cast<void*>(p), static_cast<size_t>(file_size));
            }};
        }
        // mmap 失败仍走原 pread 路径，保持低虚拟地址空间/文件系统限制下可读。
    }

    char footer_buf[Footer::kEncodedLength];
    if (!ReadAt(file.fd, footer_buf, sizeof(footer_buf), file_size - sizeof(footer_buf), mapping.get()))
        return Status::IOError("cannot read SSTable footer");
    Footer footer;
    Status s = footer.DecodeFrom(Slice(footer_buf, sizeof(footer_buf)));
    if (!s.Ok())
        return s;
    const BlockHandle ih = footer.index_handle();
    if (!ih.ValidWithin(file_size - Footer::kEncodedLength) ||
        !footer.metaindex_handle().ValidWithin(ih.offset()) || ih.size() > SIZE_MAX - 5)
        return Status::Corruption("SSTable block handle outside file");

    // 索引正文和校验尾部一次读取，校验通过后才能构造 Block。
    const size_t n = static_cast<size_t>(ih.size());
    auto bytes = std::make_unique<char[]>(n + 5);
    if (!ReadAt(file.fd, bytes.get(), n + 5, ih.offset(), mapping.get()))
        return Status::IOError("cannot read index block");
    s = CheckBlock(bytes.get(), n, bytes.get() + n);
    if (!s.Ok())
        return s;
    auto index = std::make_unique<Block>(bytes.release(), n, true);
    if (!index->status().Ok())
        return index->status();
    std::string filter;
    if (options.bloom_bits_per_key > 0) {
        s = ReadFilter(file.fd, footer.metaindex_handle(), cmp, &filter, mapping.get());
        if (!s.Ok())
            return s;
    }

    auto* rep = new Rep;
    rep->fd = file.fd;
    rep->mapping = std::move(mapping);
    rep->file_size = file_size;
    rep->index_block = index.release();
    rep->options = options;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->filter = std::move(filter);
    rep->cmp = cmp;
    // TableCache 使用稳定命名空间；独立打开时仍分配独有 ID，避免跨库串块。
    rep->cache_id = options.block_cache ? (cache_id ? cache_id : options.block_cache->NewId()) : 0;
    rep->file_number = file_number;
    *table = new Table(rep);
    file.fd = -1;
    return Status::OK();
}

// ── Table::~Table ─────────────────────────────────────────────

Table::~Table() {
    delete rep_->index_block;
    close(rep_->fd);
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
//   缓存 key   = [cache_id: 8B][file_number: 8B][block_offset: 8B][block_size: 8B]
//   缓存 value = shared_ptr<const Block>，保存已校验的不可变块
//   命中时：返回共享数据的 Block 视图，不复制字节或重复校验 restart 数组
//   未命中时：验证后将原块加入缓存，返回同样持有所有权的视图

Status Table::ReadBlock(const BlockHandle& handle, Block** result) const {
    *result = nullptr;

    // 修复：在缓存查询和内存分配之前验证范围，损坏 handle 不能读取元数据区域。
    if (!handle.ValidWithin(rep_->metaindex_handle.offset()))
        return Status::Corruption("data block handle outside data region");

    if (rep_->mapping) {
        const size_t n = static_cast<size_t>(handle.size());
        const char* data = static_cast<const char*>(rep_->mapping.get()) + handle.offset();
        Status s = CheckBlock(data, n, data + n);
        if (!s.Ok())
            return s;
        // 修改：直接使用映射字节，每次仍校验 CRC，不在块缓存中重复缓存整片映射。
        auto block = std::make_unique<Block>(data, n, false, rep_->mapping);
        if (!block->status().Ok())
            return block->status();
        *result = block.release();
        return Status::OK();
    }

    // ── Day11: 尝试 BlockCache 命中 ───────────────────────
    Cache* bc = rep_->options.block_cache;
    if (bc != nullptr && rep_->cache_id != 0) {
        // 修复：长度也参与缓存标识，防止同偏移的伪造长块命中较短的缓存而越界复制。
        char key_buf[32];
        EncodeFixed64(key_buf, rep_->cache_id);
        EncodeFixed64(key_buf + 8, rep_->file_number);
        EncodeFixed64(key_buf + 16, handle.offset());
        EncodeFixed64(key_buf + 24, handle.size());
        Slice cache_key(key_buf, sizeof(key_buf));

        Cache::Handle* ch = bc->Lookup(cache_key);
        if (ch != nullptr) {
            // 修改：先让视图持有原块，再释放缓存句柄，避免并发淘汰造成悬空指针。
            const auto* cached = static_cast<const std::shared_ptr<const Block>*>(bc->Value(ch));
            *result = new Block(*cached);
            bc->Release(ch);
            return Status::OK();
        }
    }

    // ── 磁盘读取 ──────────────────────────────────────────
    size_t n   = static_cast<size_t>(handle.size());
    // 修改：正文和物理尾部连续读取；先防止 32 位 size_t 上的分配长度回绕。
    if (n > SIZE_MAX - 5)
        return Status::Corruption("data block size overflow");
    char*  buf = new char[n + 5];

    if (!ReadAt(rep_->fd, buf, n + 5, handle.offset())) {
        delete[] buf;
        return Status::IOError("cannot read data block");
    }

    // 尾部已随正文读入，仍完整校验 type+CRC；Block 对外仅暴露正文 n 字节。
    const char* trailer = buf + n;
    Status s = CheckBlock(buf, n, trailer);
    if (!s.Ok()) {
        delete[] buf;
        return s;
    }
    Block* block = new Block(buf, n, true /* owned */);
    if (!block->status().Ok()) {
        s = block->status();
        delete block;
        return s;
    }

    // ── 将已验证的 Block 插入 BlockCache ──────────────────
    if (bc != nullptr && rep_->cache_id != 0) {
        char key_buf[32];
        EncodeFixed64(key_buf, rep_->cache_id);
        EncodeFixed64(key_buf + 8, rep_->file_number);
        EncodeFixed64(key_buf + 16, handle.offset());
        EncodeFixed64(key_buf + 24, handle.size());
        Slice cache_key(key_buf, sizeof(key_buf));

        // 修改：缓存和返回视图共同持有同一份数据，不再为缓存复制整个块。
        auto shared = std::shared_ptr<const Block>(block);
        *result = new Block(shared);
        auto* cached = new std::shared_ptr<const Block>(shared);
        Cache::Handle* ch = bc->Insert(cache_key, cached, n, [](const Slice&, void* v) {
            delete static_cast<std::shared_ptr<const Block>*>(v);
        });
        bc->Release(ch); // 释放 Insert 返回的引用
        return Status::OK();
    }

    *result = block;
    return Status::OK();
}

// ── Table::InternalGet ────────────────────────────────────────

Status Table::InternalGet(const Slice& key, void* arg,
                          void (*handle_result)(void*, const Slice&, const Slice&)) {
    Status s;

    if (!rep_->filter.empty()) {
        if (!rep_->cmp->IsValidKey(key))
            return Status::Corruption("invalid key for SSTable filter");
        // 修复：明确不存在时无需读取数据块；可能存在仍走原来的索引和精确查找。
        // KeyMayMatch 从磁盘过滤器读取探测次数，不依赖这里的 bits/key 构造参数。
        if (!BloomFilter(10).KeyMayMatch(rep_->cmp->FilterKey(key), Slice(rep_->filter)))
            return Status::OK();
    }

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
        // 修复：切换块会销毁旧迭代器，必须先保留其解析错误，供最终 status 上报。
        if (data_iter_ && status_.Ok())
            status_ = data_iter_->status();
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
