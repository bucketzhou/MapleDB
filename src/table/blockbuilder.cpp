/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-11 10:50:50
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-11 17:04:18
 * @Description:
 */
#include "table/blockbuilder.h"

#include "util/coding.h"

#include <algorithm>
#include <cassert>
namespace mapledb {

BlockBuilder::BlockBuilder(int block_restart_interval) :
    block_restart_interval_(block_restart_interval) {
    restarts_.push_back(0); // 第一个 entry 总是 restart point（offset=0）}
}
void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_  = 0;
    finished_ = false;
    last_key_.clear();
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
    assert(!finished_);
    assert(counter_ <= block_restart_interval_);
    // 有序性检查，新key必须大于上一个key
    assert(last_key_.empty() || Slice(last_key_).Compare(key) < 0);
    size_t shared = 0;
    if (counter_ < block_restart_interval_) {
        const size_t min_len = std::min(last_key_.size(), key.Size());
        while (shared < min_len && last_key_[shared] == key[shared]) {
            shared++;
        }
    } else {
        // ── Restart Point：不做前缀压缩，存完整 key ──────────
        // 记录当前 buffer_ 偏移作为 restart point
        restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
        counter_ = 0;
        // shared 保持 0，表示不共享任何前缀
    }
    const size_t non_shared = key.Size() - shared;
    // 编码三个 varint：shared_len, non_shared_len, value_len
    PutVarint32(&buffer_, static_cast<uint32_t>(shared));
    PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
    PutVarint32(&buffer_, static_cast<uint32_t>(value.Size()));

    // 写入 key 的非共享部分（delta）
    buffer_.append(key.Data() + shared, non_shared);

    // 写入 value（原样存储，不压缩）
    buffer_.append(value.Data(), value.Size());

    // 更新 last_key_
    last_key_.resize(shared);
    last_key_.append(key.Data() + shared, non_shared);
    assert(Slice(last_key_) == key);

    counter_++;
}

Slice BlockBuilder::Finish() {
    // 追加 restarts 数组（每个元素 fixed32，4 字节）
    for (uint32_t offset : restarts_) {
        PutFixed32(&buffer_, offset);
    }
    // 追加 restarts 数量（fixed32，4 字节）
    PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));

    finished_ = true;
    return Slice(buffer_);
}

size_t BlockBuilder::CurrentSizeEstimate() const {
    return buffer_.size()                        // 已编码内容
           + restarts_.size() * sizeof(uint32_t) // restarts 数组
           + sizeof(uint32_t);                   // restarts 数量字段
}

} // namespace mapledb