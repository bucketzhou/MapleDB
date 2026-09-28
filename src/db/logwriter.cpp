// src/db/log_writer.cpp

#include "db/logwriter.h"

#include "util/coding.h"

#include <cassert>
#include <cstring>
#include <mutex>
#include <unistd.h> // ← 添加这行，提供 fsync/fileno

namespace mapledb {
namespace log {

// ── 简易 CRC32 实现（教学用，LevelDB 用 crc32c 硬件加速版）────

// 标准 CRC32 查找表（IEEE 802.3 多项式 0xEDB88320）
static uint32_t crc_table[256];
// call_once 保证只初始化一次，并保证其他线程能看到完整初始化结果。
static std::once_flag crc_table_init;

static void InitCRCTable() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
}

static uint32_t CRC32(const char* data, size_t len, uint32_t init_crc = 0xFFFFFFFFu) {
    std::call_once(crc_table_init, InitCRCTable);
    uint32_t c = init_crc;
    for (size_t i = 0; i < len; i++) {
        c = crc_table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ── Writer 构造/析构 ─────────────────────────────────────────

Writer::Writer(const std::string& dest_filename) : Writer(dest_filename, 0) {
    if (!dest_)
        return;

    // 追加模式保证写入在末尾，但这里仍显式定位，以可靠地查询文件长度。
    if (fseek(dest_, 0, SEEK_END) != 0) {
        fclose(dest_);
        dest_ = nullptr;
        return;
    }
    const long size = ftell(dest_);
    if (size < 0) {
        fclose(dest_);
        dest_ = nullptr;
        return;
    }
    block_offset_ = static_cast<uint32_t>(static_cast<uint64_t>(size) % kBlockSize);
}

Writer::Writer(const std::string& dest_filename, uint64_t dest_length) :
    dest_(fopen(dest_filename.c_str(), "ab")),
    block_offset_(static_cast<uint32_t>(dest_length % kBlockSize)) {}

Writer::~Writer() {
    if (dest_) {
        fflush(dest_);
        fclose(dest_);
    }
}

// ── ComputeCRC ───────────────────────────────────────────────

uint32_t Writer::ComputeCRC(RecordType type, const char* data, size_t n) const {
    // 先用 type 字节初始化 CRC（LevelDB 同款策略：type 参与校验）
    char     type_byte = static_cast<char>(type);
    uint32_t crc       = CRC32(&type_byte, 1);
    // 再对 data 继续计算（保持 CRC 连续性需要传入中间值）
    std::call_once(crc_table_init, InitCRCTable);
    uint32_t c = crc ^ 0xFFFFFFFFu; // 反转回工作状态
    for (size_t i = 0; i < n; i++) {
        c = crc_table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ── EmitPhysicalRecord：写一条物理 Record ─────────────────────

Status Writer::EmitPhysicalRecord(RecordType type, const char* ptr, size_t length) {
    assert(length <= 0xFFFF); // length 用 2 字节表示，最大 65535
    assert(block_offset_ + kHeaderSize + length <= kBlockSize);

    // 构造 7 字节 Header
    char header[kHeaderSize];

    // CRC32（type + data）
    uint32_t crc = ComputeCRC(type, ptr, length);
    // 小端序写入 header[0..3]
    header[0] = static_cast<char>(crc & 0xFF);
    header[1] = static_cast<char>((crc >> 8) & 0xFF);
    header[2] = static_cast<char>((crc >> 16) & 0xFF);
    header[3] = static_cast<char>((crc >> 24) & 0xFF);

    // length（小端序 2 字节）
    header[4] = static_cast<char>(length & 0xFF);
    header[5] = static_cast<char>((length >> 8) & 0xFF);

    // type（1 字节）
    header[6] = static_cast<char>(type);

    // 写 Header
    if (fwrite(header, 1, kHeaderSize, dest_) != kHeaderSize) {
        return Status::IOError("WAL write header failed");
    }

    // 写 Data
    if (length > 0) {
        if (fwrite(ptr, 1, length, dest_) != length) {
            return Status::IOError("WAL write data failed");
        }
    }

    block_offset_ += static_cast<uint32_t>(kHeaderSize + length);
    return Status::OK();
}

// ── AddRecord：追加一条逻辑 Record（处理跨 Block 分片）──────────

Status Writer::AddRecord(const Slice& slice, bool sync) {
    if (!dest_)
        return Status::IOError("WAL file not open");

    const char* ptr  = slice.Data();
    size_t      left = slice.Size(); // 剩余未写字节数

    Status s;
    bool   begin = true; // 是否是第一片（决定用 FIRST 还是 MIDDLE）

    do {
        // 当前 Block 剩余可用字节数
        const int block_remaining = static_cast<int>(kBlockSize) - static_cast<int>(block_offset_);

        // 如果剩余空间连 Header 都放不下（< 7 字节），用 0 填满当前 Block
        if (block_remaining < kHeaderSize) {
            if (block_remaining > 0) {
                // 用 0 字节填充 trailer
                static const char kTrailer[kHeaderSize] = {};
                fwrite(kTrailer, 1, block_remaining, dest_);
            }
            block_offset_ = 0; // 切换到新 Block
        }

        // 当前 Block 还能放多少数据（Header 以外的部分）
        assert(kBlockSize - static_cast<int>(block_offset_) >= kHeaderSize);
        const size_t avail = static_cast<size_t>(kBlockSize - block_offset_ - kHeaderSize);

        // 本次 Record 的数据长度（不超过本 Block 可用空间）
        const size_t fragment_length = (left < avail) ? left : avail;

        // 判断 Record 类型
        RecordType type;
        const bool end = (left == fragment_length); // 是否是最后一片

        if (begin && end) {
            type = kFullType; // 完整记录，一个 Block 内搞定
        } else if (begin) {
            type = kFirstType; // 第一片
        } else if (end) {
            type = kLastType; // 最后一片
        } else {
            type = kMiddleType; // 中间片
        }

        s = EmitPhysicalRecord(type, ptr, fragment_length);
        if (!s.Ok())
            return s;

        ptr += fragment_length;
        left -= fragment_length;
        begin = false;

    } while (left > 0);

    // 可选：强制刷盘（sync 模式，牺牲性能换安全）
    if (sync) {
        fflush(dest_);
        fsync(fileno(dest_));
    }

    return Status::OK();
}

Status Writer::Flush() {
    if (!dest_)
        return Status::IOError("WAL file not open");
    fflush(dest_);
    return Status::OK();
}

} // namespace log
} // namespace mapledb
