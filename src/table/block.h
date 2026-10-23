/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-09-12 13:23:59
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-17 16:04:04
 * @Description: Block：SSTable 中一个 Block 的内存表示,提供带 Restart Point 二分查找的迭代器
 */

#pragma once

#include "mapledb/slice.h"
#include "mapledb/status.h"
#include "util/coding.h"
#include "util/comparator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mapledb {

// ── Comparator 抽象接口 ───────────────────────────────────────

// class Comparator {
// public:
//     virtual ~Comparator()                                     = default;
//     virtual int Compare(const Slice& a, const Slice& b) const = 0;
// };

// class BytewiseComparatorImpl : public Comparator {
// public:
//     int Compare(const Slice& a, const Slice& b) const override { return a.Compare(b); }
// };

// inline const Comparator* BytewiseComparator() {
//     static BytewiseComparatorImpl inst;
//     return &inst;
// }

// ════════════════════════════════════════════════════════
// Block
// ════════════════════════════════════════════════════════

class Block {
public:
    // data  : Block 字节流起始指针
    // size  : 字节流大小（不含物理尾部 type+CRC 5字节）
    // owned : true 则析构时 delete[] data
    explicit Block(const char* data, size_t size, bool owned = false,
                   const std::shared_ptr<const void>& data_owner = {});
    // 修改：缓存命中返回小型只读视图，复用已校验结构；共享所有权覆盖读者生命周期。
    explicit Block(const std::shared_ptr<const Block>& backing);
    ~Block();

    Block(const Block&)            = delete;
    Block& operator=(const Block&) = delete;

    size_t size() const { return size_; }
    Status status() const { return status_; }

    // ────────────────────────────────────────────────────
    // Block::Iter
    //
    // 语义约定（修复关键）：
    //   current_ 始终指向"当前已解码的 entry"的起始偏移。
    //   current_ == restart_offset_ 表示迭代器无效（越界）。
    //
    //   ParseCurrentEntry()：解码 current_ 位置的 entry，
    //                        填充 key_buf_ / key_ / value_。
    //   AdvanceCurrent()   ：把 current_ 推进到下一条 entry。
    //   Next()             ：先 Advance，再 ParseCurrentEntry。
    //   SeekToRestartPoint ：设置 current_ = restart[i] 的 offset，
    //                        然后必须立刻调 ParseCurrentEntry。
    // ────────────────────────────────────────────────────
    class Iter {
    public:
        Iter(const Comparator* cmp, const char* data, uint32_t restart_offset,
             uint32_t num_restarts, Status status = Status::OK()) :
            cmp_(cmp), data_(data), restart_offset_(restart_offset), num_restarts_(num_restarts),
            current_(restart_offset), // 初始无效
            restart_index_(num_restarts), status_(status) {}

        // ── 状态查询 ──────────────────────────────────────
        bool Valid() const { return current_ < restart_offset_; }

        const Slice& key() const {
            assert(Valid());
            return key_;
        }
        const Slice& value() const {
            assert(Valid());
            return value_;
        }
        Status status() const { return status_; }

        // ── 移动操作 ──────────────────────────────────────

        // 向后移动一步
        void Next() {
            assert(Valid());
            // 先把 current_ 推进到下一条 entry 的起始位置
            current_ = CalcNextEntryOffset();
            // 再解码当前位置
            ParseCurrentEntry();
        }

        // 定位到第一个 entry
        void SeekToFirst() {
            if (num_restarts_ == 0)
                return;
            // restart[0] 的 offset 就是第一条 entry 的位置
            current_       = GetRestartPoint(0);
            restart_index_ = 0;
            key_buf_.clear();
            ParseCurrentEntry();
        }

        // 定位到最后一个 entry
        void SeekToLast() {
            if (num_restarts_ == 0)
                return;
            // 先跳到最后一个 restart point
            GoToRestartPoint(num_restarts_ - 1);
            // 再从那里一直推进到最后一个有效 entry
            while (Valid()) {
                uint32_t next_off = CalcNextEntryOffset();
                if (next_off >= restart_offset_)
                    break; // 下一条已越界
                current_ = next_off;
                ParseCurrentEntry();
            }
        }

        // 定位到第一个 key >= target 的 entry
        void Seek(const Slice& target) {
            if (num_restarts_ == 0)
                return;
            if (!cmp_->IsValidKey(target)) {
                CorruptionError();
                return;
            }

            // ① 在 restarts[] 上二分：找最后一个 restart_key <= target
            uint32_t left  = 0;
            uint32_t right = num_restarts_ - 1;

            while (left < right) {
                uint32_t mid = (left + right + 1) / 2;

                // 读 restart[mid] 的 key（shared==0，可直接解码）
                uint32_t    shared, non_shared, value_length;
                const char* key_ptr =
                    DecodeEntryHeader(data_ + GetRestartPoint(mid), data_ + restart_offset_,
                                      &shared, &non_shared, &value_length);

                if (key_ptr == nullptr || shared != 0) {
                    CorruptionError();
                    return;
                }
                Slice mid_key(key_ptr, non_shared);
                if (!cmp_->IsValidKey(mid_key)) {
                    CorruptionError();
                    return;
                }

                if (cmp_->Compare(mid_key, target) < 0) {
                    left = mid; // mid 的 key < target，往右找
                } else {
                    right = mid - 1; // mid 的 key >= target，往左收
                }
            }

            // ② 从 left 对应的 restart point 顺序扫描
            GoToRestartPoint(left);
            while (Valid() && cmp_->Compare(key_, target) < 0) {
                Next();
            }
        }

        // 向前移动一步（需从上一个 restart point 重扫，性能较低）
        void Prev() {
            assert(Valid());
            const uint32_t original = current_;

            // 找到包含 original 的 restart 区间的上一个 restart point
            while (restart_index_ > 0 && GetRestartPoint(restart_index_) >= original) {
                restart_index_--;
            }
            // 如果 restart_index_==0 且其 offset >= original，说明已在第一条
            if (GetRestartPoint(restart_index_) >= original) {
                current_       = restart_offset_; // 置为无效
                restart_index_ = num_restarts_;
                return;
            }

            // 从上一个 restart point 扫，找到 original 之前的最后一个 entry
            GoToRestartPoint(restart_index_);
            while (Valid()) {
                uint32_t next_off = CalcNextEntryOffset();
                if (next_off >= original)
                    break; // 下一条 >= original，当前就是前驱
                current_ = next_off;
                ParseCurrentEntry();
            }
        }

    private:
        const Comparator* cmp_;
        const char*       data_;
        uint32_t          restart_offset_; // restarts 数组起始（也是 entries 区域上界）
        uint32_t          num_restarts_;
        uint32_t          current_;       // 当前 entry 起始偏移
        uint32_t          restart_index_; // 当前 entry 所在 restart 区间
        Slice             key_;
        Slice             value_;
        std::string       key_buf_;
        Status            status_;

        // 获取第 index 个 restart point 的 offset
        uint32_t GetRestartPoint(uint32_t index) const {
            assert(index < num_restarts_);
            return DecodeFixed32(data_ + restart_offset_ + index * sizeof(uint32_t));
        }

        // 跳到第 index 个 restart point 并解码其 entry
        // restart point 的 entry：shared=0，存完整 key
        void GoToRestartPoint(uint32_t index) {
            key_buf_.clear();
            key_           = Slice(key_buf_);
            current_       = GetRestartPoint(index);
            restart_index_ = index;
            ParseCurrentEntry();
        }

        // 解码一条 entry 的头部（3 个 varint）
        // 返回 key_delta 起始指针，失败返回 nullptr
        static const char* DecodeEntryHeader(const char* p, const char* limit, uint32_t* shared,
                                             uint32_t* non_shared, uint32_t* value_length) {
            if (p >= limit || limit - p < 3)
                return nullptr;

            // 快速路径：三个 varint 均 < 128（单字节）
            *shared       = static_cast<uint8_t>(p[0]);
            *non_shared   = static_cast<uint8_t>(p[1]);
            *value_length = static_cast<uint8_t>(p[2]);
            if ((*shared | *non_shared | *value_length) < 128) {
                p += 3;
            } else {
                if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr)
                    return nullptr;
                if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr)
                    return nullptr;
                if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr)
                    return nullptr;
            }

            // 修复：两个 uint32 长度相加可能回绕，先逐项扣减再判断，禁止越界解码。
            size_t remaining = static_cast<size_t>(limit - p);
            if (*non_shared > remaining || *value_length > remaining - *non_shared) {
                return nullptr;
            }
            return p;
        }

        // 解码 current_ 位置的 entry，填充 key_buf_/key_/value_
        // current_ 必须已经指向有效位置（调用前由移动操作保证）
        void ParseCurrentEntry() {
            if (current_ >= restart_offset_) {
                // 越界，设为无效
                current_       = restart_offset_;
                restart_index_ = num_restarts_;
                return;
            }

            const char* p     = data_ + current_;
            const char* limit = data_ + restart_offset_;

            uint32_t    shared, non_shared, value_length;
            const char* key_ptr = DecodeEntryHeader(p, limit, &shared, &non_shared, &value_length);

            if (key_ptr == nullptr || key_buf_.size() < shared) {
                CorruptionError();
                return;
            }

            // 还原完整 key：前 shared 字节保持不变，追加 non_shared 字节
            key_buf_.resize(shared);
            key_buf_.append(key_ptr, non_shared);
            key_   = Slice(key_buf_);
            value_ = Slice(key_ptr + non_shared, value_length);
            // 修复：CRC 有效也可能含非法内部键，比较或提取 tag 前必须检查格式。
            if (!cmp_->IsValidKey(key_)) {
                CorruptionError();
                return;
            }

            // 更新 restart_index_（当前 entry 属于哪个 restart 区间）
            while (restart_index_ + 1 < num_restarts_
                   && GetRestartPoint(restart_index_ + 1) <= current_) {
                ++restart_index_;
            }
        }

        // 计算当前 entry 之后下一条 entry 的起始偏移
        // 不修改 current_，只做计算
        uint32_t CalcNextEntryOffset() const {
            if (!Valid())
                return restart_offset_;

            // 修改：当前 value 的范围已由 ParseCurrentEntry 校验，其结尾就是下一条起点。
            // 复用已解码的范围，避免 Next/Prev/SeekToLast 为计算偏移再次解析旧 header。
            return static_cast<uint32_t>((value_.Data() + value_.Size()) - data_);
        }

        void CorruptionError() {
            current_       = restart_offset_;
            restart_index_ = num_restarts_;
            status_        = Status::Corruption("bad entry in block");
            key_buf_.clear();
            key_   = Slice(key_buf_);
            value_ = Slice();
        }
    }; // class Iter

    // 创建迭代器（调用方负责 delete）
    Iter* NewIterator(const Comparator* cmp) const;

private:
    const char* data_;
    size_t      size_;
    bool        owned_;
    uint32_t    num_restarts_;
    uint32_t    restart_offset_;
    Status      status_; // 非法块结构必须报告损坏，不能伪装为空块。
    std::shared_ptr<const void> data_owner_; // 映射块持有整个区域，Table 淘汰后仍可安全读取。
    std::shared_ptr<const Block> backing_; // 视图持有原块，淘汰缓存也不会使 data_ 悬空。
};

} // namespace mapledb
