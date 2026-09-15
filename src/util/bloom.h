/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-12 13:35:39
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-12 13:36:12
 * @Description:src/util/bloom.h
 * BloomFilter：概率过滤器，用极小内存快速排除"key 不存在"的情况
 * 参考 LevelDB util/bloom.cc，使用双重哈希模拟多个独立哈希函数
 */

#pragma once

#include "mapledb/slice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mapledb {

class BloomFilter {
public:
    // bits_per_key：每个 key 占多少 bit（推荐 10，误判率 ≈ 1%）
    // 越大误判率越低，但内存越多
    explicit BloomFilter(int bits_per_key = 10);

    // 根据 keys 创建过滤器，结果追加到 dst
    // dst 末尾 1 字节存储实际使用的哈希函数数量（k_）
    void CreateFilter(const std::vector<Slice>& keys, std::string* dst) const;

    // 查询 key 是否"可能存在"于 filter 中
    // filter 是 CreateFilter 生成的字节串
    // 返回 false → key 肯定不存在（无假阴性）
    // 返回 true  → key 可能存在（有假阳性，约 1%）
    bool KeyMayMatch(const Slice& key, const Slice& filter) const;

private:
    int    bits_per_key_; // 每个 key 的 bit 数
    size_t k_;            // 哈希函数数量（≈ bits_per_key * ln(2)）

    // LevelDB 同款哈希函数（MurmurHash 变体）
    static uint32_t BloomHash(const Slice& key);
};

} // namespace mapledb