/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-21 12:10:16
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-23 15:49:06
 * @Description:
 */

// TODO: 可以将state_改为 unique_ptr<char[]> state_;的形式
// TODO: 性能基本等价，但析构和移动可自动生成，降低 double free、泄漏等错误风险。
#include "mapledb/slice.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#pragma once

namespace mapledb {
class [[nodiscard]] Status {
public:
    /* ---------------------------------- 工厂方法 ---------------------------------- */

    static Status OK() { return Status(); }
    static Status NotFound(const Slice& msg1, const Slice& msg2 = Slice()) {
        return Status(KNotFound, msg1, msg2);
    }
    static Status Corruption(const Slice& msg1, const Slice& msg2 = Slice()) {
        return Status(KCorruption, msg1, msg2);
    }
    static Status IOError(const Slice& msg1, const Slice& msg2 = Slice()) {
        return Status(KIOError, msg1, msg2);
    }
    static Status InvalidArgument(const Slice& msg1, const Slice& msg2 = Slice()) {
        return Status(KInvalidArgument, msg1, msg2);
    }

    /* ---------------------------------- 构造函数 ---------------------------------- */

    // 默认构造 KOK， state_= nullptr，零开销
    Status() noexcept = default;

    ~Status() { delete[] state_; }
    // 拷贝构造，深拷贝state_
    Status(const Status& rhs) { state_ = rhs.state_ == nullptr ? nullptr : CopyState(rhs.state_); }
    // 移动构造
    Status(Status&& rhs) noexcept : state_(std::exchange(rhs.state_, nullptr)) {}
    // 拷贝赋值运算符
    Status& operator=(const Status& rhs) {
        Status tmp(rhs);
        Swap(tmp);
        return *this;
    }
    // 移动赋值运算符
    Status& operator=(Status&& rhs) noexcept {
        Swap(rhs);
        return *this;
    }
    // 交换函数
    void Swap(Status& rhs) noexcept { std::swap(state_, rhs.state_); }

    /* ---------------------------------- 查询接口 ---------------------------------- */

    [[nodiscard]] bool Ok() const noexcept { return state_ == nullptr; }
    [[nodiscard]] bool IsNotFound() const noexcept { return code() == KNotFound; }
    [[nodiscard]] bool IsCorruption() const noexcept { return code() == KCorruption; }
    [[nodiscard]] bool IsIOError() const noexcept { return code() == KIOError; }
    [[nodiscard]] bool IsInvalidArgument() const noexcept { return code() == KInvalidArgument; }
    // 转换为可读字符串，for debug
    [[nodiscard]] std::string ToString() const {
        if (!state_) {
            return "OK";
        };
        std::string res;
        switch (code()) {
            case KNotFound:
                res = "NotFound: ";
                break;
            case KCorruption:
                res = "Corruption: ";
                break;
            case KIOError:
                res = "IOError: ";
                break;
            case KInvalidArgument:
                res = "InvalidArgument: ";
                break;
            default:
                res = "Unknown: ";
                break;
        }
        std::uint32_t size = MessageSize(state_);
        memcpy(&size, state_, sizeof(size));
        res.append(state_ + 5, size);
        return res;
    }
    friend void swap(Status& lhs, Status& rhs) noexcept { lhs.Swap(rhs); }

private:
    enum Code {
        KOK              = 0,
        KNotFound        = 1,
        KCorruption      = 2,
        KIOError         = 3,
        KInvalidArgument = 4,
    };

    //   state_ 内存布局（nullptr = OK）：
    //   state_[0..3]  : uint32_t  消息总长度
    //   state_[4]     : uint8_t   错误码 Code
    //   state_[5..]   : char[]    错误消息文本
    const char* state_ = nullptr;

    [[nodiscard]] Code code() const noexcept {
        return state_ == nullptr ? KOK : static_cast<Code>(state_[4]);
    }

    // 带两段消息的构造
    Status(Code code, const Slice& msg1, const Slice& msg2) {
        assert(code != KOK);
        // 计算总消息长度（两段消息用": "连接）
        const uint32_t len1 = static_cast<uint32_t>(msg1.Size());
        const uint32_t len2 = static_cast<uint32_t>(msg2.Size());
        const uint32_t size = len1 + (len2 > 0 ? 2 + len2 : 0);

        char* result = new char[size + 5];
        memcpy(result, &size, sizeof(size));   // [0..3]: 消息长度
        result[4] = static_cast<char>(code);   // [4]:    错误码
        memcpy(result + 5, msg1.Data(), len1); // [5..]:  消息1
        if (len2 > 0) {
            result[5 + len1] = ':';
            result[6 + len1] = ' ';
            memcpy(result + 7 + len1, msg2.Data(), len2);
        }
        state_ = result;
    }
    // 深拷贝
    static const char* CopyState(const char* state) {
        uint32_t size;
        memcpy(&size, state, sizeof(size));
        char* result = new char[size + 5];
        memcpy(result, state, size + 5);
        return result;
    }

    static std::uint32_t MessageSize(const char* state) noexcept {
        uint32_t size;
        std::memcpy(&size, state, sizeof(size));
        return size;
    }
};
} // namespace mapledb
