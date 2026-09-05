/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-20 11:56:13
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-09-05 15:41:45
 * @Description: a zero copy string view, base data structure for mapledb,
 * refered to levelDB::Slice
 */

#pragma once

#include <cassert>
#include <cstring>
#include <string>

// C++17, 方便互转
#include <string_view>

namespace mapledb {

class Slice {
public:
    // 默认构造
    constexpr Slice() noexcept = default;
    // 从裸指针和长度构造
    constexpr Slice(const char* data, size_t size) noexcept : data_(data), size_(size) {}
    // 从C风格字符串构造
    Slice(const char* data) : data_(data), size_(strlen(data)) {}
    // 从std::string构造，不拷贝，需确保str在slice生命周期存活
    Slice(const std::string& str) noexcept : data_(str.data()), size_(str.size()) {}
    // 从std::string_view构造
    constexpr Slice(std::string_view sv) noexcept : data_(sv.data()), size_(sv.size()) {}

    // 拷贝/赋值构造函数，默认，（浅拷贝，只复制指针和长度）
    Slice(const Slice&) noexcept            = default;
    Slice& operator=(const Slice&) noexcept = default;

    // 删除临时string对象构造Slice
    // Slice(std::string&&) = delete;

    // 接口
    [[nodiscard]] const char* Data() const noexcept { return data_; }
    [[nodiscard]] size_t      Size() const noexcept { return size_; }
    [[nodiscard]] bool        Empty() const noexcept { return size_ == 0; }
    // 支持随机访问

    constexpr char operator[](size_t i) const noexcept {
        assert(i < size_);
        return data_[i];
    }

    // 移除前缀
    constexpr void RemovePrefix(size_t n) noexcept {
        assert(n <= size_);
        data_ += n;
        size_ -= n;
    }

    // 转换为std::string
    // TODO:返回对象还是引用
    [[nodiscard]] std::string ToString() const { return std::string(data_, size_); }
    // 转换为std::string_view
    [[nodiscard]] std::string_view ToStringView() const noexcept {
        return std::string_view(data_, size_);
    }

    // 逐字符比较
    [[nodiscard]] int Compare(const Slice& rhs) const noexcept {
        size_t    prefix = size_ <= rhs.size_ ? size_ : rhs.size_;
        const int res    = std::memcmp(data_, rhs.data_, prefix);
        if (res == 0) {
            if (size_ < rhs.size_) {
                return -1;
            }
            if (size_ > rhs.size_) {
                return 1;
            }
        }
        return res;
    }
    // 判断是否以prefix开头
    [[nodiscard]] bool StartsWith(const Slice& prefix) const noexcept {
        if (prefix.size_ > size_)
            return false;
        int res = std::memcmp(prefix.data_, data_, prefix.size_);
        if (res == 0) {
            return true;
        }
        return false;
    }

    // 比较运算符
    bool operator==(const Slice& rhs) const noexcept {
        return size_ == rhs.size_ && memcmp(data_, rhs.data_, size_) == 0;
    }
    bool operator!=(const Slice& rhs) const noexcept { return !(*this == rhs); }
    bool operator<(const Slice& rhs) const noexcept { return Compare(rhs) < 0; }
    bool operator<=(const Slice& rhs) const noexcept { return Compare(rhs) <= 0; }
    bool operator>(const Slice& rhs) const noexcept { return Compare(rhs) > 0; }
    bool operator>=(const Slice& rhs) const noexcept { return Compare(rhs) >= 0; }

private:
    const char* data_ = "";
    size_t      size_ = 0;
};

} // namespace mapledb