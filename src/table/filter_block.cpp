#include "table/filter_block.h"

#include "db/dbformat.h"
#include "util/coding.h"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <typeinfo>

namespace mapledb {
static constexpr unsigned kFilterBaseLg = 11;

bool FilterKeyModeForComparator(const Comparator* cmp, FilterKeyMode* mode) {
    if (cmp && typeid(*cmp) == typeid(InternalKeyComparator)) {
        *mode = FilterKeyMode::kInternalKey;
        return true;
    }
    if (cmp && typeid(*cmp) == typeid(BytewiseComparatorImpl)) {
        *mode = FilterKeyMode::kUserKey;
        return true;
    }
    // 未知比较器可能把不同字节的 key 判为相等，不能安全使用字节 hash。
    return false;
}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
    const uint64_t index = block_offset >> kFilterBaseLg;
    assert(index >= offsets_.size());
    while (index > offsets_.size()) GenerateFilter();
}

void FilterBlockBuilder::AddKey(const Slice& key) {
    if (keys_.empty() || Slice(keys_.back()) != key)
        keys_.push_back(key.ToString()); // 相邻版本的相同 user key 只计算一次。
}

void FilterBlockBuilder::GenerateFilter() {
    if (result_.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("filter block exceeds fixed32 offset range");
    offsets_.push_back(static_cast<uint32_t>(result_.size()));
    if (keys_.empty()) return; // 没有 DataBlock 起始位置的区间记为空过滤器。
    std::vector<Slice> slices;
    slices.reserve(keys_.size());
    for (const auto& key : keys_) slices.emplace_back(key);
    policy_.CreateFilter(slices, &result_);
    keys_.clear();
}

Slice FilterBlockBuilder::Finish() {
    if (!keys_.empty()) GenerateFilter();
    if (result_.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("filter block exceeds fixed32 offset range");
    const auto array_offset = static_cast<uint32_t>(result_.size());
    for (uint32_t offset : offsets_) PutFixed32(&result_, offset);
    PutFixed32(&result_, array_offset);
    result_.push_back(static_cast<char>(kFilterBaseLg));
    return Slice(result_);
}

FilterBlockReader::FilterBlockReader(const Slice& contents) {
    const size_t n = contents.Size();
    if (n < 5 || static_cast<uint8_t>(contents[n - 1]) != kFilterBaseLg) return;
    array_offset_ = DecodeFixed32(contents.Data() + n - 5);
    if (array_offset_ > n - 5 || (n - 5 - array_offset_) % 4 != 0) return;
    count_ = (n - 5 - array_offset_) / 4;
    data_ = contents.Data();
    offsets_ = data_ + array_offset_;
    uint32_t previous = 0;
    for (size_t i = 0; i <= count_; ++i) {
        const uint32_t offset = DecodeFixed32(offsets_ + i * 4);
        if (offset < previous || offset > array_offset_) return;
        // 非空 Bloom 至少有 64 bit 和一个探测次数字节。
        if (i > 0 && offset != previous && offset - previous < 9) return;
        previous = offset;
    }
    if (count_ > 0 && DecodeFixed32(offsets_) != 0) return;
    valid_ = true;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) const {
    const uint64_t index = block_offset >> kFilterBaseLg;
    if (!valid_ || index >= count_) return true;
    const uint32_t start = DecodeFixed32(offsets_ + index * 4);
    const uint32_t limit = DecodeFixed32(offsets_ + (index + 1) * 4);
    if (start == limit) return false;
    return policy_.KeyMayMatch(key, Slice(data_ + start, limit - start));
}
} // namespace mapledb
