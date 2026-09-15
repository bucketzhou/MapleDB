/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-12 13:45:56
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-12 13:46:29
 * @Description:
 */

#include "util/bloom.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace mapledb {

// ── BloomHash：LevelDB 同款 MurmurHash 变体 ──────────────────

uint32_t BloomFilter::BloomHash(const Slice& key) {
    const char*    data = key.Data();
    size_t         n    = key.Size();
    const uint32_t seed = 0xbc9f1d34;
    const uint32_t m    = 0xc6a4a793;
    const uint32_t r    = 24;

    uint32_t h = seed ^ static_cast<uint32_t>(n * m);

    // 每次处理 4 字节
    while (n >= 4) {
        uint32_t w;
        memcpy(&w, data, 4);
        data += 4;
        n -= 4;
        h += w;
        h *= m;
        h ^= (h >> 16);
    }

    // 处理剩余字节
    switch (n) {
        case 3:
            h += static_cast<uint8_t>(data[2]) << 16;
            [[fallthrough]];
        case 2:
            h += static_cast<uint8_t>(data[1]) << 8;
            [[fallthrough]];
        case 1:
            h += static_cast<uint8_t>(data[0]);
            h *= m;
            h ^= (h >> r);
            break;
    }
    return h;
}

// ── 构造函数 ─────────────────────────────────────────────────

BloomFilter::BloomFilter(int bits_per_key) : bits_per_key_(bits_per_key) {
    // k = bits_per_key * ln(2) ≈ bits_per_key * 0.693
    // 向下取整减少探测开销（LevelDB 同款策略）
    k_ = static_cast<size_t>(bits_per_key * 0.69);
    if (k_ < 1)
        k_ = 1;
    if (k_ > 30)
        k_ = 30; // 最多 30 个哈希函数
}

// ── CreateFilter ─────────────────────────────────────────────

void BloomFilter::CreateFilter(const std::vector<Slice>& keys, std::string* dst) const {
    size_t n = keys.size();

    // 计算 bit 数组大小
    size_t bits = n * static_cast<size_t>(bits_per_key_);
    if (bits < 64)
        bits = 64; // 保底 64 bit，避免极少 key 时误判率过高

    size_t bytes = (bits + 7) / 8;
    bits         = bytes * 8; // 对齐到字节边界

    // 在 dst 末尾追加 bytes 字节（初始化为 0）
    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);

    // 末尾追加 1 字节：记录实际使用的 k 值（用于 KeyMayMatch 时读取）
    dst->push_back(static_cast<char>(k_));

    // 为每个 key 设置 k_ 个 bit
    char* array = &(*dst)[init_size];
    for (const Slice& key : keys) {
        uint32_t h = BloomHash(key);
        // 双重哈希：循环右移 17 位产生 delta，模拟多个哈希函数
        // 参考 [Kirsch, Mitzenmacher 2006]
        const uint32_t delta = (h >> 17) | (h << 15);
        for (size_t j = 0; j < k_; j++) {
            const uint32_t bitpos = h % static_cast<uint32_t>(bits);
            array[bitpos / 8] |= (1 << (bitpos % 8));
            h += delta;
        }
    }
}

// ── KeyMayMatch ──────────────────────────────────────────────

bool BloomFilter::KeyMayMatch(const Slice& key, const Slice& bloom_filter) const {
    const size_t len = bloom_filter.Size();
    if (len < 2)
        return false; // 至少要有 1 字节数据 + 1 字节 k 值

    const char* array = bloom_filter.Data();
    // 最后 1 字节是 k_（哈希函数数量）
    const size_t bits = (len - 1) * 8;
    const size_t k    = static_cast<uint8_t>(array[len - 1]);

    // 防御：k 值异常时直接返回可能存在（避免假阴性）
    if (k > 30)
        return true;

    uint32_t       h     = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);

    for (size_t j = 0; j < k; j++) {
        const uint32_t bitpos = h % static_cast<uint32_t>(bits);
        if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) {
            return false; // 某个 bit 为 0 → 肯定不存在
        }
        h += delta;
    }
    return true; // 所有 bit 都是 1 → 可能存在
}

} // namespace mapledb