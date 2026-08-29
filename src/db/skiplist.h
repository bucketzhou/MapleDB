/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-25 19:38:39
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-29 09:52:42
 * @Description: skiplist 实现
 */
#pragma once

#include "util/arena.h"
#include "util/random.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>

// 线程安全语义：
//   写（Insert）：需要外部互斥（同时只有一个写者）
//   读（Contains / Iterator）：无锁，多线程并发安全
//   前提：读期间 SkipList 不会被销毁（由调用方保证）

namespace mapledb {
// ────────────────────────────────────────────────────────
// SkipList 模板类
//
// Key：节点存储的键类型
// Comparator：比较器，需要提供 operator()(Key a, Key b) -> int
//             返回值：<0 表示 a<b，0 表示 a==b，>0 表示 a>b
//
template <typename Key, class Comparator>
class SkipList {
private:
    // 前向声明
    struct Node;

public:
    // 构造函数
    // cmp：key 比较器（按值传入，通常很小）
    // arena：节点内存从这里分配，arena 的生命周期必须 >= SkipList
    explicit SkipList(Comparator cmp, Arena* arena);
    // 禁止拷贝（SkipList 含有大量内部状态，拷贝没有意义）
    SkipList(const SkipList&)            = delete;
    SkipList& operator=(const SkipList&) = delete;

    // 插入 key
    // 要求：当前列表中没有与 key 相等的元素（MemTable 由上层保证）
    // 线程安全：写操作需要外部同步
    void Insert(const Key& key);

    // 查询 key 是否存在
    // 线程安全：无锁，可并发调用
    bool Contains(const Key& key) const;

    // ── 迭代器 ──────────────────────────────────────────────
    class Iterator {
    public:
        explicit Iterator(const SkipList* list);

        // 当前位置是否有效（指向一个真实节点）
        bool Valid() const;

        // 返回当前节点的 key（Valid() 必须为 true）
        const Key& key() const;

        // 向后移动一步（到下一个更大的 key）
        void Next();

        // 向前移动一步（到上一个更小的 key）
        void Prev();

        // 定位到第一个 key >= target 的节点
        void Seek(const Key& target);

        // 定位到第一个节点（最小 key）
        void SeekToFirst();

        // 定位到最后一个节点（最大 key）
        void SeekToLast();

    private:
        const SkipList* list_;
        Node*           node_; // nullptr 表示无效（越界）
    };

private:
    // ── 常量 ────────────────────────────────────────────────
    // 跳表最大层数（LevelDB 使用 12，经验值）
    // 每层概率 1/4，12 层理论上可以高效索引约 4^12 = 1600 万个元素
    enum { kMaxHeight = 12 };

    // Node定义
    struct Node {
    public:
        explicit Node(const Key& k) : key(k) {}
        // 节点存储的key，不可变
        const Key key;
        // 获取第 n 层的后继节点
        // 使用 acquire 语义：确保读者能看到完整的节点初始化

        Node* Next(int n) {
            assert(n >= 0 && n < kMaxHeight);
            return next_[n].load(std::memory_order_acquire);
        }
        void SetNext(int n, Node* next) {
            assert(n >= 0 && n < kMaxHeight);
            next_[n].store(next, std::memory_order_release);
        }
        Node* NoBarrierNext(int n) {
            assert(n >= 0 && n < kMaxHeight);
            return next_[n].load(std::memory_order_relaxed);
        }
        void NoBarrierSetNext(int n, Node* next) {
            assert(n >= 0 && n < kMaxHeight);
            next_[n].store(next, std::memory_order_relaxed);
        }

    private:
        // next_[0] 是 Level 0（最底层）的后继指针
        // 声明为 next_[1] 是占位，实际分配时会多申请 (height-1) 个
        // 这是 C++ 的"柔性数组"技巧（flexible array member 的 C++ 版本）
        std::atomic<Node*> next_[1];
    };

    Comparator const compare_;
    Arena* const     arena_;
    // 哨兵头节点（不存储真实数据，height=kMaxHeight）
    // head_->next_[i] 是第 i 层的第一个真实节点
    Node* const head_;
    // 当前跳表的实际最大高度（从 1 开始）
    // atomic：写者用 relaxed store 更新，读者用 relaxed load 读取
    // 读者读到"过时"的值也没关系（只会多走一层，不影响正确性）
    std::atomic<int> max_height_;
    // 随机数生成器（用于决定新节点的高度）
    Random rnd_;

    // 返回当前最大高度（inline，热路径）
    [[nodiscard]] inline int GetMaxHeight() const {
        return max_height_.load(std::memory_order_relaxed);
    }
    // 生成一个随机高度（1 到 kMaxHeight）
    // 概率：P(height=1) = 3/4, P(height=2) = 3/16, ...
    [[nodiscard]] int RandomHeight() noexcept;
    // 创建一个新的 Node，从 Arena 分配内存
    [[nodiscard]] Node* NewNode(const Key& key, int height);
    // 比较两个 key（封装 compare_ 的调用）
    [[nodiscard]] bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }
    // Key是否大于给定Node的Key
    [[nodiscard]] bool KeyIsAfterNode(const Key& key, Node* node) const {
        return (node != nullptr) && compare_(key, node->key) > 0;
    }
    [[nodiscard]] Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
    // 找到最后一个 key < target 的节点（用于反向查找）
    [[nodiscard]] Node* FindLessThan(const Key& key) const;
    // 找到最后一个节点 最大Key
    [[nodiscard]] Node* FindLast() const noexcept;
};

// ────────────────────────────────────────────────────────
// 以下是模板类的实现（必须放在头文件）
// ────────────────────────────────────────────────────────
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int        height) {
    assert(height > 0 && height <= kMaxHeight);
    // 计算需要分配的字节数：
    //   sizeof(Node) 包含 Node::key 和 next_[0]（1个原子指针）
    //   额外分配 (height-1) 个原子指针的空间

    std::size_t size = sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1);
    assert(arena_ != nullptr);
    char* const mem = arena_->AllocateAligned(size);

    // placement new：在 Arena 分配的内存上构造 Node 对象
    // 不会调用 operator new，不产生额外的堆分配
    return new (mem) Node(key);
}
// ── RandomHeight ────────────────────────────────────────
template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() noexcept {
    static const unsigned int kBranching = 4;
    int                       height     = 1;
    while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
        height++;
    }
    return height;
}
// ── FindGreaterOrEqual ───────────────────────────────────
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x     = head_;
    int   level = GetMaxHeight() - 1; // 从最高层开始

    while (true) {
        Node* next = x->Next(level); // 当前层的下一个节点（acquire load）

        if (KeyIsAfterNode(key, next)) {
            // key > next->key：在当前层继续向右走
            x = next;
        } else {
            // key <= next->key（或 next 为 nullptr）：需要下降一层
            if (prev != nullptr) {
                prev[level] = x; // 记录该层的前驱节点
            }
            if (level == 0) {
                // 已到最底层，next 就是目标（或第一个 >= key 的节点）
                return next;
            }
            // 下降一层继续搜索
            level--;
        }
    }
}

// ── FindLessThan ────────────────────────────────────────
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
    Node* x     = head_;
    int   level = GetMaxHeight() - 1;

    while (true) {
        assert(x == head_ || compare_(x->key, key) < 0);
        Node* next = x->Next(level);

        if (next == nullptr || compare_(next->key, key) >= 0) {
            // next 不存在，或 next->key >= key：下降一层
            if (level == 0) {
                return x; // x 是最后一个 < key 的节点
            }
            level--;
        } else {
            // next->key < key：在当前层向右走
            x = next;
        }
    }
}

// ── FindLast ────────────────────────────────────────────
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const noexcept {
    Node* x     = head_;
    int   level = GetMaxHeight() - 1;

    while (true) {
        Node* next = x->Next(level);
        if (next == nullptr) {
            if (level == 0) {
                return x; // x 是最后一个节点（可能是 head_，即列表为空）
            }
            level--;
        } else {
            x = next;
        }
    }
}

// ── 构造函数 ─────────────────────────────────────────────
template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena) :
    compare_(cmp), arena_(arena), head_(NewNode(Key{} /* 哨兵 key，不使用 */, kMaxHeight)),
    max_height_(1), rnd_(0xdeadbeef) { // 固定种子，可重现
    // 初始化头节点的所有层后继为 nullptr
    for (int i = 0; i < kMaxHeight; i++) {
        head_->SetNext(i, nullptr);
    }
}

// ── Insert ──────────────────────────────────────────────
template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
    // 第一步：找到插入位置，同时收集每层的前驱节点
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    // SkipList 不允许插入相同的 key（由上层 MemTable 保证）
    assert(x == nullptr || !Equal(key, x->key));

    // 第二步：随机决定新节点的高度
    int height = RandomHeight();

    if (height > GetMaxHeight()) {
        // 新节点高度超过当前最大高度：
        // 对于超出部分的层，前驱节点设为 head_（这些层目前只有 head_）
        for (int i = GetMaxHeight(); i < height; i++) {
            prev[i] = head_;
        }
        // 更新最大高度（relaxed 即可：读者读到过时值只是性能小损失）
        max_height_.store(height, std::memory_order_relaxed);
    }

    // 第三步：创建新节点
    x = NewNode(key, height);

    // 第四步：从 Level 0 到 Level height-1，逐层插入
    for (int i = 0; i < height; i++) {
        // ① 设置新节点的后继（NoBarrier 即可）
        //    因为 ② 的 SetNext（release store）会提供内存屏障，
        //    确保在 ② 之前，① 的写入对其他线程可见
        x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));

        // ② 更新前驱节点的后继指针（release store）
        //    读者一旦通过 acquire load 读到 x，就能看到 x 的完整内容
        prev[i]->SetNext(i, x);
    }
}

// ── Contains ────────────────────────────────────────────
template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return (x != nullptr && Equal(key, x->key));
}

// ── Iterator 实现 ────────────────────────────────────────

template <typename Key, class Comparator>
SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) : list_(list), node_(nullptr) {
    assert(list != nullptr);
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Iterator::Valid() const {
    return node_ != nullptr;
}

template <typename Key, class Comparator>
const Key& SkipList<Key, Comparator>::Iterator::key() const {
    assert(Valid());
    return node_->key;
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::Next() {
    assert(Valid());
    node_ = node_->Next(0); // Level 0 是完整的有序链表
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::Prev() {
    assert(Valid());
    // SkipList 没有反向指针，需要从 head_ 重新搜索
    node_ = list_->FindLessThan(node_->key);
    if (node_ == list_->head_) {
        node_ = nullptr; // 已经到达最前面
    }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
    node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
    node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::SeekToLast() {
    node_ = list_->FindLast();
    if (node_ == list_->head_) {
        node_ = nullptr; // 空列表
    }
}

} // namespace mapledb