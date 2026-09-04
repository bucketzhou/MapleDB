/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-03 19:21:16
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-04 10:13:51
 * @Description:
 */
#include "memtable.h"

#include <cassert>

namespace mapledb {

// ── 构造/析构 ──────────────────────────────────────────────

MemTable::MemTable(const InternalKeyComparator& comparator) :
    comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() {
    assert(refs_ == 0);
    // arena_ 在析构时自动释放所有 SkipList 节点的内存
    // table_ 的析构函数不会 delete 节点（它只是清空头节点，节点归 Arena 管）
}

// ── Add：写入一条 kv 记录 ─────────────────────────────────

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
    // 编码格式（这段字节全部存入 Arena，SkipList 节点的 key 指向它）：
    //
    //  ┌──────────────────┬─────────────────────────────┬──────────────────┐
    //  │ internal_key_len │    internal_key              │    value         │
    //  │  (varint32)      │  user_key + tag(8 bytes)     │  len + content   │
    //  └──────────────────┴─────────────────────────────┴──────────────────┘
    //
    // internal_key_len = user_key.size() + 8（tag 固定 8 字节）

    size_t key_size          = key.Size();
    size_t val_size          = value.Size();
    size_t internal_key_size = key_size + 8; // user_key + tag

    // 计算编码后总长度（varint 最多 5 字节）
    // 格式：varint32(internal_key_size) + internal_key + varint32(val_size) + value
    const size_t encoded_len =
        VarintLength(internal_key_size) + internal_key_size + VarintLength(val_size) + val_size;

    // 从 Arena 分配内存（一次性分配，不碎片化）
    char* buf = arena_.Allocate(encoded_len);

    // ① 写 varint32(internal_key_size)
    char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));

    // ② 写 user_key
    memcpy(p, key.Data(), key_size);
    p += key_size;

    // ③ 写 tag = (seq << 8) | type（固定 8 字节小端）
    uint64_t tag = (static_cast<uint64_t>(seq) << 8) | static_cast<uint64_t>(type);
    EncodeFixed64(p, tag);
    p += 8;

    // ④ 写 varint32(val_size) + value
    p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
    memcpy(p, value.Data(), val_size);
    p += val_size;

    assert(p == buf + encoded_len); // 验证编码长度正确

    // 插入 SkipList（buf 就是 MemTable key，SkipList 只存指针，不拷贝）
    table_.Insert(buf);
}

// ── Get：查询一条 kv 记录 ─────────────────────────────────

bool MemTable::Get(const LookupKey& lkey, std::string* value, Status* s) const {
    // 获取 MemTable key（带 varint 前缀的完整查找键）
    Slice memkey = lkey.memtable_key();

    Table::Iterator iter(&table_);

    // Seek 到第一个 >= memkey 的位置
    // 由于 InternalKeyComparator 的排序规则：
    //   相同 user_key 按 seq 降序，所以 Seek(user_key + kMaxSeq) 后，
    //   第一条 user_key 匹配的记录就是最新版本
    iter.Seek(memkey.Data());

    if (!iter.Valid()) {
        // 没有任何 key >= memkey，user_key 肯定不存在
        return false;
    }

    // 解码当前条目的 internal_key，检查 user_key 是否匹配
    // iter.key() 返回的是 const char*（带 varint 前缀的 memtable key）
    const char* entry = iter.key();

    // 解码 internal_key_len
    uint32_t    internal_key_len;
    const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &internal_key_len);
    assert(key_ptr != nullptr);

    // 提取 user_key（internal_key 的前 key_len-8 字节）
    Slice entry_user_key(key_ptr, internal_key_len - 8);
    Slice lookup_user_key = lkey.user_key();

    // 比较 user_key（字节级比较）
    if (entry_user_key.Compare(lookup_user_key) != 0) {
        // user_key 不匹配，说明 user_key 根本不存在于此 MemTable
        return false;
    }

    // user_key 匹配！解码 tag，判断类型
    uint64_t  tag  = DecodeFixed64(key_ptr + internal_key_len - 8);
    ValueType type = static_cast<ValueType>(tag & 0xff);

    switch (type) {
        case kTypeValue: {
            // 正常写入：解码 value
            // value 紧跟在 internal_key 后面（以 varint32 长度开头）
            const char* val_ptr = key_ptr + internal_key_len;
            uint32_t    val_len;
            const char* val_start = GetVarint32Ptr(val_ptr, val_ptr + 5, &val_len);
            assert(val_start != nullptr);
            value->assign(val_start, val_len);
            *s = Status::OK();
            return true; // 找到
        }

        case kTypeDeletion:
            // 删除标记：key 存在但已被删除
            *s = Status::NotFound(lookup_user_key);
            return true; // 返回 true 表示"在此 MemTable 找到了答案"（答案是：已删除）
    }

    // 不应该到达这里
    return false;
}

// ── NewIterator ───────────────────────────────────────────

MemTable::Table::Iterator* MemTable::NewIterator() { return new Table::Iterator(&table_); }
} // namespace mapledb
