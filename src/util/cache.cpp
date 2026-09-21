/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-21 07:52:08
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-21 07:57:03
 * @Description:
 */
#include "util/cache.h"

#include "util/coding.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mapledb {

// ════════════════════════════════════════════════════════════════
// LRUHandle
// ════════════════════════════════════════════════════════════════

struct LRUHandle {
    void* value;
    void (*deleter)(const Slice&, void*);
    LRUHandle* next_hash;   // 哈希链（单向）
    LRUHandle* next;        // LRU 双向链表
    LRUHandle* prev;        // LRU 双向链表
    size_t     charge;      // 该条目的容量消耗
    size_t     key_length;  // key 数据长度
    bool       in_cache;    // 是否还在哈希表里
    uint32_t   refs;        // 引用计数
    uint32_t   hash;        // key 的哈希值（避免重复计算）
    char       key_data[1]; // key 数据起始（柔性数组）

    Slice key() const {
        // dummy head 节点的 next 指向自身，不调用此函数
        return Slice(key_data, key_length);
    }
};

// ════════════════════════════════════════════════════════════════
// HandleTable — 拉链法哈希表
// ════════════════════════════════════════════════════════════════

class HandleTable {
public:
    HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
    ~HandleTable() { delete[] list_; }

    // 查找 key/hash 对应的 LRUHandle，未找到返回 nullptr
    LRUHandle* Lookup(const Slice& key, uint32_t hash) { return *FindPointer(key, hash); }

    // 插入 h；若已有相同 key 的条目则替换，返回被替换的旧条目（或 nullptr）
    LRUHandle* Insert(LRUHandle* h) {
        LRUHandle** ptr = FindPointer(h->key(), h->hash);
        LRUHandle*  old = *ptr;
        h->next_hash    = (old == nullptr ? nullptr : old->next_hash);
        *ptr            = h;
        if (old == nullptr) {
            ++elems_;
            if (elems_ > length_)
                Resize();
        }
        return old;
    }

    // 删除 key/hash 对应的条目，返回被删除的条目（或 nullptr）
    LRUHandle* Remove(const Slice& key, uint32_t hash) {
        LRUHandle** ptr    = FindPointer(key, hash);
        LRUHandle*  result = *ptr;
        if (result != nullptr) {
            *ptr = result->next_hash;
            --elems_;
        }
        return result;
    }

private:
    uint32_t    length_; // 桶数量（始终是 2 的幂次）
    uint32_t    elems_;  // 当前元素数量
    LRUHandle** list_;   // 桶数组

    // 返回指向 key/hash 对应槽位的指针（可能是链尾的 nullptr）
    LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
        LRUHandle** ptr = &list_[hash & (length_ - 1)];
        while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
            ptr = &(*ptr)->next_hash;
        }
        return ptr;
    }

    // 扩容（桶数量翻倍），重新散列所有元素
    void Resize() {
        uint32_t new_length = 4;
        while (new_length < elems_)
            new_length *= 2;

        LRUHandle** new_list = new LRUHandle*[new_length]();
        uint32_t    count    = 0;
        for (uint32_t i = 0; i < length_; i++) {
            LRUHandle* h = list_[i];
            while (h != nullptr) {
                LRUHandle* next = h->next_hash;
                uint32_t   slot = h->hash & (new_length - 1);
                h->next_hash    = new_list[slot];
                new_list[slot]  = h;
                h               = next;
                count++;
            }
        }
        assert(elems_ == count);
        delete[] list_;
        list_   = new_list;
        length_ = new_length;
    }
};

// ════════════════════════════════════════════════════════════════
// LRUCache — 单分片
// ════════════════════════════════════════════════════════════════

class LRUCache {
public:
    LRUCache() : capacity_(0), usage_(0) {
        // 初始化两个循环双向链表的哑头节点
        lru_.next    = &lru_;
        lru_.prev    = &lru_;
        in_use_.next = &in_use_;
        in_use_.prev = &in_use_;
    }

    ~LRUCache() {
        // in_use_ 链表应为空（调用方应先 Release 所有 Handle）
        assert(in_use_.next == &in_use_);
        // 释放 lru_ 链表中所有条目
        for (LRUHandle* e = lru_.next; e != &lru_;) {
            LRUHandle* next = e->next;
            assert(e->in_cache);
            e->in_cache = false;
            assert(e->refs == 1);
            Unref(e);
            e = next;
        }
    }

    void SetCapacity(size_t cap) { capacity_ = cap; }

    // ── Insert ─────────────────────────────────────────────────
    Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value, size_t charge,
                          void (*deleter)(const Slice&, void*)) {
        // 分配 LRUHandle（key 数据紧跟在结构体后）
        LRUHandle* e  = static_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.Size()));
        e->value      = value;
        e->deleter    = deleter;
        e->charge     = charge;
        e->key_length = key.Size();
        e->hash       = hash;
        e->in_cache   = false;
        e->refs       = 1; // 给调用方的那个引用
        memcpy(e->key_data, key.Data(), key.Size());

        if (capacity_ > 0) {
            e->refs++; // 缓存自身的引用
            e->in_cache = true;
            LRU_Append(&in_use_, e);
            usage_ += charge;
            // 若 key 已存在则替换旧条目（减少其缓存引用）
            FinishErase(table_.Insert(e));
        } else {
            // capacity_ == 0：不缓存，e->next = nullptr 作为标记
            e->next = nullptr;
        }

        // 超出容量时从 lru_ 尾部淘汰
        while (usage_ > capacity_ && lru_.next != &lru_) {
            LRUHandle* old = lru_.next;
            assert(old->refs == 1);
            bool erased = FinishErase(table_.Remove(old->key(), old->hash));
            assert(erased);
            (void) erased;
        }

        return reinterpret_cast<Cache::Handle*>(e);
    }

    // ── Lookup ─────────────────────────────────────────────────
    Cache::Handle* Lookup(const Slice& key, uint32_t hash) {
        LRUHandle* e = table_.Lookup(key, hash);
        if (e != nullptr)
            Ref(e);
        return reinterpret_cast<Cache::Handle*>(e);
    }

    // ── Release ────────────────────────────────────────────────
    void Release(Cache::Handle* handle) { Unref(reinterpret_cast<LRUHandle*>(handle)); }

    // ── Erase ──────────────────────────────────────────────────
    void Erase(const Slice& key, uint32_t hash) { FinishErase(table_.Remove(key, hash)); }

    // ── Prune ──────────────────────────────────────────────────
    void Prune() {
        while (lru_.next != &lru_) {
            LRUHandle* e = lru_.next;
            assert(e->refs == 1);
            bool erased = FinishErase(table_.Remove(e->key(), e->hash));
            assert(erased);
            (void) erased;
        }
    }

    size_t TotalCharge() const { return usage_; }

private:
    // 将 e 从链表中摘除
    void LRU_Remove(LRUHandle* e) {
        e->next->prev = e->prev;
        e->prev->next = e->next;
    }

    // 将 e 插到 list 的头部前面（MRU 端）
    void LRU_Append(LRUHandle* list, LRUHandle* e) {
        e->next       = list;
        e->prev       = list->prev;
        e->prev->next = e;
        e->next->prev = e;
    }

    // 增加引用计数；若条目在 lru_ 中（refs 原为 1），移到 in_use_
    void Ref(LRUHandle* e) {
        if (e->in_cache && e->refs == 1) {
            LRU_Remove(e);
            LRU_Append(&in_use_, e);
        }
        e->refs++;
    }

    // 减少引用计数：
    //   refs == 0 → 调用 deleter，释放内存
    //   refs == 1 且 in_cache → 移回 lru_ 链表（可淘汰）
    void Unref(LRUHandle* e) {
        assert(e->refs > 0);
        e->refs--;
        if (e->refs == 0) {
            assert(!e->in_cache);
            (*e->deleter)(e->key(), e->value);
            free(e);
        } else if (e->in_cache && e->refs == 1) {
            // 从 in_use_ 移到 lru_ 尾部（MRU 端）
            LRU_Remove(e);
            LRU_Append(&lru_, e);
        }
    }

    // 把 e 从缓存中逐出（从链表摘除、清标志、减引用）
    // 返回 e != nullptr
    bool FinishErase(LRUHandle* e) {
        if (e != nullptr) {
            assert(e->in_cache);
            LRU_Remove(e);
            e->in_cache = false;
            usage_ -= e->charge;
            Unref(e);
        }
        return e != nullptr;
    }

    size_t      capacity_;
    size_t      usage_;
    LRUHandle   lru_;    // 可淘汰链表（dummy head）；lru_.next 是 LRU 端
    LRUHandle   in_use_; // 被持有链表（dummy head）
    HandleTable table_;
};

static uint32_t HashSlice(const Slice& s) {
    const char* data = s.Data();
    size_t      len  = s.Size();
    uint32_t    seed = 0xbc9f1d34u;
    uint32_t    h    = seed ^ static_cast<uint32_t>(len);

    while (len >= 4) {
        uint32_t w = (static_cast<uint32_t>(static_cast<uint8_t>(data[0])))
                     | (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8)
                     | (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16)
                     | (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
        data += 4;
        len -= 4;
        w *= 0xcc9e2d51u;
        w = (w >> 17) | (w << 15);
        w *= 0x1b873593u;
        h ^= w;
        h = (h >> 19) | (h << 13);
        h = h * 5 + 0xe6546b64u;
    }
    switch (len) {
        case 3:
            h ^= static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16;
            [[fallthrough]];
        case 2:
            h ^= static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8;
            [[fallthrough]];
        case 1:
            h ^= static_cast<uint32_t>(static_cast<uint8_t>(data[0]));
            h *= 0xcc9e2d51u;
    }
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// ════════════════════════════════════════════════════════════════
// ShardedLRUCache — 16 分片，减少锁争用
// ════════════════════════════════════════════════════════════════

static constexpr int kNumShardBits = 4;
static constexpr int kNumShards    = 1 << kNumShardBits; // 16

class ShardedLRUCache : public Cache {
public:
    explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
        // 均匀分配容量到每个分片
        const size_t per_shard = (capacity + kNumShards - 1) / kNumShards;
        for (int i = 0; i < kNumShards; i++) {
            shard_[i].SetCapacity(per_shard);
        }
    }

    ~ShardedLRUCache() override = default;

    Handle* Insert(const Slice& key, void* value, size_t charge,
                   void (*deleter)(const Slice&, void*)) override {
        const uint32_t              hash = HashSlice(key);
        const uint32_t              s    = Shard(hash);
        std::lock_guard<std::mutex> lk(mu_[s]);
        return shard_[s].Insert(key, hash, value, charge, deleter);
    }

    Handle* Lookup(const Slice& key) override {
        const uint32_t              hash = HashSlice(key);
        const uint32_t              s    = Shard(hash);
        std::lock_guard<std::mutex> lk(mu_[s]);
        return shard_[s].Lookup(key, hash);
    }

    void Release(Handle* handle) override {
        LRUHandle*                  h = reinterpret_cast<LRUHandle*>(handle);
        const uint32_t              s = Shard(h->hash);
        std::lock_guard<std::mutex> lk(mu_[s]);
        shard_[s].Release(handle);
    }

    void* Value(Handle* handle) override { return reinterpret_cast<LRUHandle*>(handle)->value; }

    void Erase(const Slice& key) override {
        const uint32_t              hash = HashSlice(key);
        const uint32_t              s    = Shard(hash);
        std::lock_guard<std::mutex> lk(mu_[s]);
        shard_[s].Erase(key, hash);
    }

    uint64_t NewId() override {
        std::lock_guard<std::mutex> lk(id_mu_);
        return ++last_id_;
    }

    void Prune() override {
        for (int i = 0; i < kNumShards; i++) {
            std::lock_guard<std::mutex> lk(mu_[i]);
            shard_[i].Prune();
        }
    }

    size_t TotalCharge() const override {
        size_t total = 0;
        for (int i = 0; i < kNumShards; i++) {
            // 近似值，不加锁（教学环境无并发问题）
            total += shard_[i].TotalCharge();
        }
        return total;
    }

private:
    static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

    LRUCache           shard_[kNumShards];
    mutable std::mutex mu_[kNumShards];
    std::mutex         id_mu_;
    uint64_t           last_id_;
};

// ── 工厂函数 ─────────────────────────────────────────────────────

Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

} // namespace mapledb