/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-19 17:51:09
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-19 17:51:27
 * @Description:
 * LRU Cache 接口定义Cache 是一个抽象接口，支持 Insert / Lookup / Release / Erase 操作。
 * NewLRUCache(capacity) 返回一个 16 分片的 LRU Cache 实例。
 * 设计参考 LevelDB cache.h
 */

#pragma once

#include "mapledb/slice.h"

#include <cstddef>
#include <cstdint>

namespace mapledb {

class Cache {
public:
    Cache()                        = default;
    virtual ~Cache()               = default;
    Cache(const Cache&)            = delete;
    Cache& operator=(const Cache&) = delete;

    // 不透明 Handle，指向缓存中的某个条目
    struct Handle {};

    // 将 key/value 插入缓存，charge 表示该条目占用的"容量单位"（通常是字节数）。
    // 返回一个 Handle，调用方**必须**在不再使用时调用 Release(handle)。
    // 若 key 已存在，旧条目会被替换（旧条目的 deleter 在引用归零后被调用）。
    // deleter 在条目被从缓存中淘汰且引用计数归零时被调用，用于释放 value。
    virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                           void (*deleter)(const Slice& key, void* value)) = 0;

    // 查找 key 对应的条目。未命中返回 nullptr。
    // 命中时返回 Handle，调用方**必须**调用 Release(handle)。
    virtual Handle* Lookup(const Slice& key) = 0;

    // 释放由 Lookup 或 Insert 返回的 Handle。
    // 当引用计数归零且条目已被 Erase 时，会调用 deleter。
    virtual void Release(Handle* handle) = 0;

    // 返回 handle 所指向的 value（在 Release 前始终有效）
    virtual void* Value(Handle* handle) = 0;

    // 淘汰 key 对应的条目（若存在）。
    // 已有 Handle 仍然有效，直到 Release 被调用。
    virtual void Erase(const Slice& key) = 0;

    // 返回一个新的数字 ID，可用于构造复合缓存 key（如 [id][offset]）
    virtual uint64_t NewId() = 0;

    // 淘汰所有当前未被持有（引用计数为 1，即仅缓存自身持有）的条目
    virtual void Prune() {}

    // 返回缓存中所有条目的 charge 总和
    virtual size_t TotalCharge() const = 0;
};

// 创建一个容量为 capacity 的 LRU 缓存（16 分片，线程安全）
Cache* NewLRUCache(size_t capacity);

} // namespace mapledb