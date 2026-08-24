/*
 * @Author: bucketzhou zhouyangcumt@163.com
 * @Date: 2026-08-24 14:00:21
 * @LastEditors: bucketzhou zhouyangcumt@163.com
 * @LastEditTime: 2026-08-24 15:14:47
 * @Description: 参考 LevelDB::Random，用于 SkipList 节点高度的随机化,整个实现只有一个 .h 文件，全部
 * inline，极致轻量
 */
#pragma once

#include <cassert>
#include <cstdint>

namespace mapledb {
class Random {
public:
    explicit Random(uint32_t seed) : seed_(seed & 0x7fffffffu) {
        if (seed_ == 0 || seed_ == 2147483647L) {
            seed_ = 1;
        }
    }
    // 生成下一个随机数，范围 [1, 2^31-2]
    //
    // LCG 公式：seed = (seed * A) mod M
    //   A = 16807  (= 7^5)
    //   M = 2147483647 (= 2^31 - 1，Mersenne 素数)
    //
    // mod 操作优化：利用 (2^31-1) 是 Mersenne 素数的性质
    //   x mod (2^31-1) = (x >> 31) + (x & 0x7fffffff)
    //   当结果 >= 2^31-1 时，减去 2^31-1
    //   这样避免了昂贵的除法运算

    uint32_t Next() {
        static const uint32_t M = 2147483647L; // 2^31-1
        static const uint64_t A = 16807;       // 7^5

        // 用 uint64_t 计算乘积，避免溢出
        uint64_t product = seed_ * A;

        // 利用 Mersenne 素数性质优化 mod 运算
        seed_ = static_cast<uint32_t>((product >> 31) + (product & M));

        // 如果结果 >= M，减去 M（等价于 mod M）
        if (seed_ >= M) {
            seed_ -= M;
        }
        return seed_;
    }

    // 生成 [0, n-1] 范围内的均匀随机数
    // 注意：n 必须大于 0
    uint32_t Uniform(int n) {
        assert(n > 0);
        return Next() % static_cast<uint32_t>(n);
    }

    // 以 1/n 的概率返回 true
    // 用法：if (rnd.OneIn(4)) { /* 25% 概率执行 */ }
    // SkipList 的 RandomHeight 用这个：每层有 1/4 概率继续升高
    bool OneIn(int n) { return Uniform(n) == 0; }

    // 生成一个在 [lo, hi] 区间内均匀分布的随机数（包含两端）
    uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }

private:
    uint32_t seed_;
};
} // namespace mapledb