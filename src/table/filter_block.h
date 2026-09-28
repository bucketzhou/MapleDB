#pragma once

#include "util/bloom.h"
#include "util/comparator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mapledb {

// 自有 metadata 名称，避免与其他 hash/校验和格式的过滤器混用。
inline constexpr const char* kBloomFilterMetaKey = "filter.mapledb.BloomFilter.v1";
enum class FilterKeyMode : uint8_t { kUserKey = 0, kInternalKey = 1 };
bool FilterKeyModeForComparator(const Comparator* cmp, FilterKeyMode* mode);

// 每个 2 KiB 文件区间一个过滤器，DataBlock 以起始 offset 选择区间。
// 编码：[filters][fixed32 offsets][fixed32 array_offset][uint8 base_lg]。
class FilterBlockBuilder {
public:
    explicit FilterBlockBuilder(int bits_per_key) : policy_(bits_per_key) {}
    void StartBlock(uint64_t block_offset);
    void AddKey(const Slice& user_key);
    Slice Finish();
private:
    void GenerateFilter();
    BloomFilter policy_;
    std::vector<std::string> keys_;
    std::vector<uint32_t> offsets_;
    std::string result_;
};

// contents 必须在 reader 生命周期内有效。损坏或未知编码保守返回可能存在。
class FilterBlockReader {
public:
    explicit FilterBlockReader(const Slice& contents);
    bool KeyMayMatch(uint64_t block_offset, const Slice& user_key) const;
private:
    BloomFilter policy_;
    const char* data_ = nullptr;
    const char* offsets_ = nullptr;
    uint32_t array_offset_ = 0;
    size_t count_ = 0;
    bool valid_ = false;
};

} // namespace mapledb
