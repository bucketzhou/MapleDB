#pragma once
#include "util/coding.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace mapledb::crc32 {
// 修改：读写端共用线程安全 IEEE CRC32；previous 是已取反的上一段结果，支持增量拼接。
inline uint32_t Extend(const char* data, size_t len, uint32_t previous = 0) {
    // 修复：并发读表会同时校验 CRC，局部静态初始化保证查表只构造一次且没有竞争。
    static const auto table = [] {
        // 修改：slicing-by-8 预计算各字节位置的贡献，保持原 IEEE CRC32 多项式。
        std::array<std::array<uint32_t, 256>, 8> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            result[0][i] = c;
        }
        for (size_t slice = 1; slice < result.size(); ++slice)
            for (size_t i = 0; i < 256; ++i) {
                const uint32_t c = result[slice - 1][i];
                result[slice][i] = result[0][c & 0xFF] ^ (c >> 8);
            }
        return result;
    }();
    uint32_t c = previous ^ 0xFFFFFFFFu;
    // 每轮合并 8 字节，减少逐字节依赖链；DecodeFixed32 用 memcpy，允许非对齐地址。
    while (len >= 8) {
        const uint32_t first = DecodeFixed32(data) ^ c;
        const uint32_t last = DecodeFixed32(data + 4);
        c = table[7][first & 0xFF] ^ table[6][(first >> 8) & 0xFF] ^
            table[5][(first >> 16) & 0xFF] ^ table[4][first >> 24] ^
            table[3][last & 0xFF] ^ table[2][(last >> 8) & 0xFF] ^
            table[1][(last >> 16) & 0xFF] ^ table[0][last >> 24];
        data += 8;
        len -= 8;
    }
    while (len-- > 0)
        c = table[0][(c ^ static_cast<uint8_t>(*data++)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace mapledb::crc32

