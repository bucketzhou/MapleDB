/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-04 16:45:51
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-04 16:46:31
 * @Description:
 */

#include "db/writebatch.h"

#include "db/dbformat.h"
#include "db/memtable.h"
#include "util/coding.h"

#include <cassert>

namespace mapledb {

// ── 头部访问辅助（直接读写 rep_ 的固定偏移）────────────────

static uint64_t ReadSequence(const std::string& rep) {
    return DecodeFixed64(rep.data()); // [0..7]
}
static void     WriteSequence(std::string* rep, uint64_t seq) { EncodeFixed64(&(*rep)[0], seq); }
static uint32_t ReadCount(const std::string& rep) {
    return DecodeFixed32(rep.data() + 8); // [8..11]
}
static void WriteCount(std::string* rep, uint32_t n) { EncodeFixed32(&(*rep)[8], n); }

// ── 构造/Clear ───────────────────────────────────────────────

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
    rep_.clear();
    rep_.resize(kHeader, 0); // 用 0 填充 12 字节头部
}

// ── 头部字段的 public 访问接口 ───────────────────────────────

void     WriteBatch::SetSequence(uint64_t seq) { WriteSequence(&rep_, seq); }
uint64_t WriteBatch::Sequence() const { return ReadSequence(rep_); }
int      WriteBatch::Count() const { return static_cast<int>(ReadCount(rep_)); }

// ── Put / Delete：追加操作到 rep_ ────────────────────────────

void WriteBatch::Put(const Slice& key, const Slice& value) {
    // 操作条数 +1
    WriteCount(&rep_, ReadCount(rep_) + 1);

    // 追加：tag(1B) + key(varstring) + value(varstring)
    rep_.push_back(static_cast<char>(kTypeValue));
    PutLengthPrefixedSlice(&rep_, key);
    PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
    WriteCount(&rep_, ReadCount(rep_) + 1);

    // 追加：tag(1B) + key(varstring)（Delete 没有 value）
    rep_.push_back(static_cast<char>(kTypeDeletion));
    PutLengthPrefixedSlice(&rep_, key);
}

// ── Append：合并两个 batch ───────────────────────────────────

void WriteBatch::Append(const WriteBatch& source) {
    // 更新操作总数
    WriteCount(&rep_, ReadCount(rep_) + ReadCount(source.rep_));

    assert(source.rep_.size() >= kHeader);
    // 把 source 的 records 部分（跳过 12 字节头部）追加到 this
    rep_.append(source.rep_.data() + kHeader, source.rep_.size() - kHeader);
}

// ── Iterate：遍历 rep_ 中的所有操作，回调 handler ─────────────

Status WriteBatch::Iterate(Handler* handler) const {
    Slice input(rep_);
    if (input.Size() < kHeader) {
        return Status::Corruption("malformed WriteBatch (too small)");
    }

    input.RemovePrefix(kHeader); // 跳过 12 字节头部

    Slice key, value;
    int   found = 0;

    while (!input.Empty()) {
        found++;

        // 读取 tag（操作类型，1 字节）
        uint8_t tag = static_cast<uint8_t>(input[0]);
        input.RemovePrefix(1);

        switch (tag) {
            case kTypeValue:
                // Put：key + value
                if (!GetLengthPrefixedSlice(&input, &key)
                    || !GetLengthPrefixedSlice(&input, &value)) {
                    return Status::Corruption("bad WriteBatch Put");
                }
                handler->Put(key, value);
                break;

            case kTypeDeletion:
                // Delete：只有 key
                if (!GetLengthPrefixedSlice(&input, &key)) {
                    return Status::Corruption("bad WriteBatch Delete");
                }
                handler->Delete(key);
                break;

            default:
                return Status::Corruption("unknown WriteBatch tag");
        }
    }

    if (found != Count()) {
        return Status::Corruption("WriteBatch has wrong count");
    }

    return Status::OK();
}

// ── InsertInto：把 batch 的操作写入 MemTable ──────────────────

// 内部 Handler 实现：将操作逐条 Add 到 MemTable
namespace {
class MemTableInserter : public WriteBatch::Handler {
public:
    SequenceNumber sequence_;
    MemTable*      mem_;

    void Put(const Slice& key, const Slice& value) override {
        mem_->Add(sequence_++, kTypeValue, key, value);
    }
    void Delete(const Slice& key) override { mem_->Add(sequence_++, kTypeDeletion, key, Slice()); }
};
} // namespace

Status WriteBatch::InsertInto(MemTable* memtable) const {
    MemTableInserter inserter;
    inserter.sequence_ = Sequence();
    inserter.mem_      = memtable;
    return Iterate(&inserter);
}

void WriteBatch::DecodeFrom(const Slice& contents) {
    rep_.assign(contents.Data(), contents.Size());
}

} // namespace mapledb