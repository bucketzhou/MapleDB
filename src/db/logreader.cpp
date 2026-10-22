// src/db/log_reader.cpp

#include "db/logreader.h"

#include "util/coding.h"

#include <cassert>
#include <cstring>

namespace mapledb {
namespace log {

// ── CRC32（与 Writer 保持一致的实现）───────────────────────────

static uint32_t crc_table_r[256];
static bool     crc_table_r_init = false;

static void InitCRCTableR() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table_r[i] = c;
    }
    crc_table_r_init = true;
}

static uint32_t CRC32R(const char* data, size_t len, uint32_t init = 0xFFFFFFFFu) {
    if (!crc_table_r_init)
        InitCRCTableR();
    uint32_t c = init;
    for (size_t i = 0; i < len; i++) {
        c = crc_table_r[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t ComputeCRCR(uint8_t type, const char* data, size_t n) {
    char     type_byte = static_cast<char>(type);
    uint32_t crc       = CRC32R(&type_byte, 1);
    // 反转回中间工作状态继续计算
    uint32_t c = crc ^ 0xFFFFFFFFu;
    if (!crc_table_r_init)
        InitCRCTableR();
    for (size_t i = 0; i < n; i++) {
        c = crc_table_r[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ── 构造/析构 ─────────────────────────────────────────────────

Reader::Reader(const std::string& src_filename, Reporter* reporter, bool checksum) :
    src_(nullptr), reporter_(reporter), checksum_(checksum), backing_store_(new char[kBlockSize]),
    buffer_(), eof_(false), last_record_offset_(0), end_of_buffer_offset_(0) {
    src_ = fopen(src_filename.c_str(), "rb");
    if (!src_)
        status_ = Status::IOError("cannot open log: " + src_filename);
}

Reader::~Reader() {
    delete[] backing_store_;
    if (src_)
        fclose(src_);
}

void Reader::ReportCorruption(size_t bytes, const char* reason) {
    if (status_.Ok())
        status_ = Status::Corruption(reason);
    if (reporter_) {
        reporter_->Corruption(bytes, Status::Corruption(reason));
    }
}

// ── ReadPhysicalRecord：读一条物理 Record ─────────────────────

unsigned int Reader::ReadPhysicalRecord(Slice* result) {
    while (true) {
        // 如果 buffer_ 里的数据不够一个 Header（7字节），需要读新 Block
        if (buffer_.Size() < static_cast<size_t>(kHeaderSize)) {
            if (!eof_) {
                // 从文件读一个完整的 Block 到 backing_store_
                size_t n = fread(backing_store_, 1, kBlockSize, src_);
                end_of_buffer_offset_ += n;
                // 修复：短读既可能是 EOF，也可能是 I/O 故障，必须检查 ferror。
                if (ferror(src_)) {
                    status_ = Status::IOError("cannot read log");
                    buffer_ = Slice();
                    eof_ = true;
                    return kEof;
                }
                eof_ = n < kBlockSize;
                if (n == 0) {
                    eof_    = true;
                    buffer_ = Slice();
                    return kEof;
                }
                buffer_ = Slice(backing_store_, n);
            } else {
                // 文件末尾，但 buffer_ 里还有不足 7 字节的 trailer，跳过
                buffer_ = Slice();
                return kEof;
            }
            continue; // 重新判断 buffer_.Size()
        }

        // 解析 Header
        const char* header = buffer_.Data();

        // 读取 length（小端序 2 字节）
        const uint16_t a      = static_cast<uint8_t>(header[4]);
        const uint16_t b      = static_cast<uint8_t>(header[5]);
        const size_t   length = a | (static_cast<size_t>(b) << 8);

        // 读取 type
        const uint8_t type = static_cast<uint8_t>(header[6]);

        // 校验总长度是否合理
        if (kHeaderSize + length > buffer_.Size()) {
            // 只有最后一个短块允许未完成尾记录；完整块内的越界长度属于损坏。
            size_t drop = buffer_.Size();
            buffer_     = Slice();
            if (!eof_ || length > kBlockSize - kHeaderSize)
                ReportCorruption(drop, "bad record length");
            eof_        = true;
            return kEof;
        }

        // 跳过 kZeroType（trailer 填充）
        if (type == kZeroType && length == 0) {
            buffer_ = Slice(); // 丢弃该 Block 剩余内容
            continue;
        }

        // CRC32 校验
        if (checksum_) {
            // 从 header[0..3] 读取存储的 CRC
            uint32_t expected_crc =
                static_cast<uint32_t>(static_cast<uint8_t>(header[0]))
                | (static_cast<uint32_t>(static_cast<uint8_t>(header[1])) << 8)
                | (static_cast<uint32_t>(static_cast<uint8_t>(header[2])) << 16)
                | (static_cast<uint32_t>(static_cast<uint8_t>(header[3])) << 24);

            // 计算实际 CRC（type + data）
            uint32_t actual_crc = ComputeCRCR(type, header + kHeaderSize, length);

            if (actual_crc != expected_crc) {
                size_t drop = buffer_.Size();
                buffer_     = Slice();
                ReportCorruption(drop, "checksum mismatch");
                return kBadRecord;
            }
        }

        // 消耗这条 Record（推进 buffer_）
        buffer_.RemovePrefix(kHeaderSize + length);

        // 返回 data 部分
        *result = Slice(header + kHeaderSize, length);
        return type;
    }
}

// ── ReadRecord：读一条完整逻辑 Record（处理分片拼接）─────────────

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
    if (!src_ || !status_.Ok())
        return false;

    scratch->clear();
    // record->clear();
    *record                   = Slice();
    bool in_fragmented_record = false; // 是否正在拼接分片记录
    uint64_t fragmented_offset = 0;

    while (true) {
        Slice        fragment;
        unsigned int record_type = ReadPhysicalRecord(&fragment);

        // 记录上一条完整记录的起始位置
        uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.Size()
                                          - static_cast<uint64_t>(kHeaderSize) - fragment.Size();

        switch (record_type) {
            case kFullType:
                if (in_fragmented_record) {
                    // 协议错误：之前有未完成的分片
                    ReportCorruption(scratch->size(), "partial record without end");
                    scratch->clear();
                    return false;
                }
                // 完整记录，直接返回
                last_record_offset_ = physical_record_offset;
                last_record_end_offset_ = end_of_buffer_offset_ - buffer_.Size();
                *record             = fragment;
                return true;

            case kFirstType:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "partial record without end");
                    scratch->clear();
                    return false;
                }
                // 分片第一段：存入 scratch，等待后续片段
                scratch->assign(fragment.Data(), fragment.Size());
                in_fragmented_record = true;
                fragmented_offset = physical_record_offset;
                break;

            case kMiddleType:
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.Size(), "missing start of fragmented record");
                } else {
                    scratch->append(fragment.Data(), fragment.Size());
                }
                break;

            case kLastType:
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.Size(), "missing start of fragmented record");
                } else {
                    scratch->append(fragment.Data(), fragment.Size());
                    // 所有分片拼完了，返回完整记录
                    last_record_offset_ = fragmented_offset;
                    last_record_end_offset_ = end_of_buffer_offset_ - buffer_.Size();
                    *record             = Slice(*scratch);
                    return true;
                }
                break;

            case kEof:
                if (in_fragmented_record) {
                    // 文件在分片中间结束，可能是崩溃时写一半
                    // 中断写入的未完成尾记录不可见，但不应把完整的先前记录判为损坏。
                    scratch->clear();
                }
                return false; // 文件正常结束

            case kBadRecord:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "error in middle of record");
                    scratch->clear();
                    in_fragmented_record = false;
                }
                return false; // 修复：完整记录校验失败必须停止，不能静默跳过继续恢复。

            default:
                ReportCorruption(fragment.Size(), "unknown record type");
                break;
        }
        if (!status_.Ok())
            return false;
    }
}

} // namespace log
} // namespace mapledb
