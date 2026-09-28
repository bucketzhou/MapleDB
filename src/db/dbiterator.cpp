/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-27 17:33:03
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-28 07:51:10
 * @Description:DBIter 实现：用户级迭代器，过滤 tombstone，去重版本，快照隔离
 */

#include "db/dbiterator.h"

#include "util/coding.h"

#include <cassert>
#include <cstring>

namespace mapledb {

DBIter::DBIter(Iterator* input, SequenceNumber sequence, const InternalKeyComparator& icmp) :
    input_(input), sequence_(sequence), icmp_(icmp), valid_(false) {}

DBIter::~DBIter() { delete input_; }

bool DBIter::Valid() const { return valid_; }

const Slice& DBIter::key() const {
    assert(valid_);
    return key_slice_;
}

const Slice& DBIter::value() const {
    assert(valid_);
    return value_slice_;
}

Status DBIter::status() const { return input_->status(); }

// SeekToFirst：找第一个对用户可见的 kv
void DBIter::SeekToFirst() {
    input_->SeekToFirst();
    FindNextUserEntry(false, "");
}

// Seek(target)：target 是 user_key
// 构造 InternalKey = user_key + (sequence_ << 8 | kValueTypeForSeek)，
// 然后 Seek 内部迭代器，再找第一个可见条目。
void DBIter::Seek(const Slice& target) {
    // 构造查找用的 InternalKey
    std::string ikey;
    ikey.reserve(target.Size() + 8);
    ikey.append(target.Data(), target.Size());
    char     tag_buf[8];
    uint64_t tag = (sequence_ << 8) | static_cast<uint64_t>(kValueTypeForSeek);
    EncodeFixed64(tag_buf, tag);
    ikey.append(tag_buf, 8);

    input_->Seek(Slice(ikey));
    FindNextUserEntry(false, "");
}

// Next：跳过当前 user_key 的所有剩余版本，找下一个不同的可见 user_key
void DBIter::Next() {
    assert(valid_);
    // 传入当前 user_key 作为 skip，FindNextUserEntry 会从 input_ 当前位置开始
    // 跳过所有 user_key <= saved_key_ 的条目（包括当前位置）
    FindNextUserEntry(true, saved_key_);
}

// ── FindNextUserEntry ────────────────────────────────────────────
// 核心逻辑：从 input_ 当前位置（含）向前推进，找到下一个用户可见条目。
//
// 过滤规则：
//   1. seq > sequence_：快照之后的写入，不可见，跳过
//   2. skipping && user_key <= skip_key：版本去重/tombstone 跳跃，跳过
//   3. kTypeDeletion：记录 user_key 为新的 skip，继续向后跳过旧版本
//   4. kTypeValue：找到可见 kv，更新 saved_key_/saved_value_ 并返回

void DBIter::FindNextUserEntry(bool skipping, const std::string& skip_key) {
    valid_               = false;
    std::string cur_skip = skip_key; // 当前需要跳过的 user_key

    while (input_->Valid()) {
        const Slice ikey = input_->key();

        // InternalKey 格式校验
        if (ikey.Size() < 8) {
            input_->Next();
            continue;
        }

        Slice          user_key = ExtractUserKey(ikey);
        SequenceNumber seq      = GetSequenceNumber(ikey);
        ValueType      type     = GetValueType(ikey);

        // ① 快照过滤
        if (seq > sequence_) {
            input_->Next();
            continue;
        }

        // ② 版本去重 / tombstone 跳跃
        if (skipping) {
            // InternalKeyComparator 保证相同 user_key 按 seq 降序排列，
            // 所以向前迭代时 user_key 只会 >= cur_skip。
            // 若 user_key == cur_skip，说明是旧版本或被删除的版本，跳过。
            int cmp = Slice(cur_skip).Compare(user_key);
            if (cmp >= 0) {
                // user_key <= cur_skip → 跳过
                input_->Next();
                continue;
            }
            // user_key > cur_skip → 进入新 key，停止跳过
            skipping = false;
        }

        // ③ 处理当前 entry
        if (type == kTypeDeletion) {
            // 该 user_key 被删除：记录为新的 skip，跳过后续所有版本
            cur_skip.assign(user_key.Data(), user_key.Size());
            skipping = true;
            input_->Next();
            continue;
        }

        // ④ kTypeValue：有效 kv，保存并返回
        saved_key_.assign(user_key.Data(), user_key.Size());
        saved_value_.assign(input_->value().Data(), input_->value().Size());
        key_slice_   = Slice(saved_key_);
        value_slice_ = Slice(saved_value_);
        valid_       = true;
        return;
    }

    // input_ 已耗尽，没有更多可见 kv
    valid_ = false;
}

} // namespace mapledb