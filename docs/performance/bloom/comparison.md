# 接入 Bloom 后：MapleDB 与 LevelDB 性能对比

测试日期：2026-09-28。以下数字均来自实际运行；两个引擎同时启用 **10 bits/key Bloom Filter**。另用同一份当前源码重测关闭过滤器的对照组，避免把不同代码版本或不同时间的结果当成 Bloom 收益。

## 1. 两边启用 Bloom 后的结果

数据规模为 10 万和 100 万条，每种配置各 5 轮。单前台线程，16 B key + 100 B value，8 MiB 显式 BlockCache，无块压缩，读取先预热一次。表中吞吐量是 5 轮中位数，单位 **万次/秒**；扫描单位为万条/秒。倍率为 LevelDB / MapleDB，超过 1 表示 LevelDB 更快。

| 负载 | 10 万条 MapleDB | 10 万条 LevelDB（倍率） | 100 万条 MapleDB | 100 万条 LevelDB（倍率） |
|---|---:|---:|---:|---:|
| 顺序写，Put 返回 | 38.01 | 42.65（1.12×） | 28.90 | 41.21（1.43×） |
| 随机写，Put 返回 | 33.01 | 35.71（1.08×） | 19.39 | 26.54（1.37×） |
| 顺序 Get | 188.67 | 169.63（0.90×） | 174.16 | 151.90（0.87×） |
| 随机 Get，键存在 | 89.87 | 113.06（1.26×） | 49.85 | 53.81（1.08×） |
| 热点 Get | 178.76 | 162.66（0.91×） | 157.58 | 142.88（0.91×） |
| 不存在键 Get | 300.36 | 265.60（0.88×） | 255.35 | 234.54（0.92×） |
| 全量扫描 | 1067.04 | 1648.18（1.54×） | 1075.04 | 1599.75（1.49×） |

百万条数据下，MapleDB 的顺序点查、热点点查、不存在键点查中位数分别比 LevelDB 高约 **14.7%、10.3%、8.9%**。LevelDB 的随机命中点查中位数高约 **7.9%**，全量扫描高约 **48.8%**。这些结论限定于上述本机预热负载。

写入还应计入最后的 Flush 和自然后台压缩：

| 规模 | 负载 | MapleDB 完整耗时中位数 | LevelDB 完整耗时中位数 | MapleDB 万次/s | LevelDB 万次/s | LevelDB / MapleDB |
|---|---|---:|---:|---:|---:|---:|
| 10 万条 | 顺序写 | 372.94 ms | 247.12 ms | 26.81 | 40.47 | 1.51× |
| 10 万条 | 随机写 | 418.39 ms | 290.53 ms | 23.90 | 34.42 | 1.44× |
| 100 万条 | 顺序写 | 3675.40 ms | 2435.29 ms | 27.21 | 41.06 | 1.51× |
| 100 万条 | 随机写 | 6811.78 ms | 4029.32 ms | 14.68 | 24.82 | 1.69× |

Bloom 接入后，写入和扫描仍是 MapleDB 的主要差距。完整的逐轮范围、抽样延迟和最终 SST 统计见 [启用结果汇总](summary-on.md)。

## 2. Bloom 本身带来了多少变化

下面比较的是**同版本、同引擎**的开关结果，单位为万次/秒。百分比为吞吐量变化，而非耗时变化。

| 规模 | 引擎 | 不存在键：关闭 → 启用 | 吞吐量变化 | 随机命中：关闭 → 启用 | 吞吐量变化 |
|---|---|---:|---:|---:|---:|
| 10 万条 | MapleDB | 185.93 → 300.36 | +61.5% | 93.16 → 89.87 | -3.5% |
| 10 万条 | LevelDB | 175.24 → 265.60 | +51.6% | 117.31 → 113.06 | -3.6% |
| 100 万条 | MapleDB | 171.61 → 255.35 | +48.8% | 48.02 → 49.85 | +3.8% |
| 100 万条 | LevelDB | 161.85 → 234.54 | +44.9% | 82.73 → 53.81 | -35.0% |

### 不存在键查询收益明确

Bloom 在 index 确定候选块后检查 user key，判定不可能存在时跳过 DataBlock 的读取和块内查找。本测试查询已有偶数键之间的奇数键，多数仍在 SST 的 key 范围内，能够体现这个路径的收益。

百万条时，MapleDB 关闭和开启的吞吐范围分别为 **168.58–174.03 万次/s**、**253.01–256.50 万次/s**；LevelDB 分别为 **152.17–164.30 万次/s**、**232.70–239.07 万次/s**。两个引擎的开关范围均明显分离。

### 随机命中查询的差距缩小，需要看双方变化

百万条随机命中点查的 LevelDB / MapleDB 从关闭时 **1.72×** 变为开启时 **1.08×**。其中 MapleDB 中位数仅增加 3.8%，开关范围大量重叠；LevelDB 中位数下降 35.0%，开关范围分别为 **68.47–82.84 万次/s**和 **50.84–56.75 万次/s**。

因此，这个差距缩小主要伴随 LevelDB 在此负载下的下降，不能表述为 MapleDB 随机读因 Bloom 大幅加速。LevelDB 的抽样 P99 也从约 1.67 µs 上升到 23.50 µs；它是每 257 次抽样的估计，需要进一步定位。

对于实际包含目标键的块，Bloom 仍需计算 hash、探测位数组，并继续读取与查找块。过滤器可能排除其他候选块，但对命中块增加了一段检查。额外 CPU/内存访问、mmap 页访问和后台活动是需要 profile 的方向；本次未采集调用栈、页错误、缓存命中或后台压缩时间线，不能确定下降的具体原因，也不能推广到其他机器、数据形态或 LevelDB 版本。

### 命中读、写入和扫描的变化

- 百万条顺序点查：MapleDB -1.6%，LevelDB -3.5%；热点点查：MapleDB -2.1%，LevelDB -3.5%。检查过滤器有额外成本，这些小幅变化中也包含运行波动。
- 百万条完整顺序/随机写：MapleDB +0.5%/+1.1%，LevelDB +1.1%/+1.8%。10 万条 MapleDB 完整随机写为 -3.8%。过滤器构建发生在落盘/压缩中；本实验未隔离这部分耗时，小幅正向变化不能解释成 Bloom 加速写入。
- 全量扫描不调用 Bloom 过滤检查。百万条 MapleDB +0.6%，LevelDB -3.8%，应视为两配置运行条件、文件布局和文件大小等共同影响的观测结果，不能称为过滤查询带来的扫描加速或减速。
- 最终 SST 大小增加约 **1.31%**。百万条 MapleDB 随机写为 109.71 → 111.14 MiB，LevelDB 为 109.83 → 111.27 MiB。该数字不包含 WAL/MANIFEST，也不代表累计写放大或进程内存。

全部负载的开关中位数、范围和 SST 增量见 [Bloom 效果汇总](effects.md)，关闭配置的引擎对比见 [关闭结果汇总](summary-off.md)。

## 3. 数据、参数和计时规则

| 项目 | 两边共同设置 |
|---|---|
| 数据 | 10 万 / 100 万条；16 B 补零十进制偶数 key；100 B 确定性二进制 value；seed=42 |
| 顺序/随机写 | 同一组 key，每个 key 只写一次；随机写为同一无重复随机排列 |
| 读取数据集 | 随机写形成的完整数据集；剩余 MemTable 已刷出，初始自然后台任务已排空 |
| 读请求 | 顺序 N 次 Get；相同随机排列 N 次 Get；前 1% key 均匀热点 N 次；穿插奇数键 N 次；全量扫描 N 条 |
| 内存与块 | WriteBuffer 4 MiB；DataBlock 4 KiB；restart interval=16；SST 目标 2 MiB；显式 BlockCache 8 MiB |
| 文件缓存 | MapleDB 100 个 Table；LevelDB max_open_files=110，扣除非 Table 配额后为 100 |
| 压缩 / Bloom | 块压缩关闭；同版本分别测 0 和 10 bits/key |
| 编译和线程 | 两库 C++17、Release、-O2 -DNDEBUG；相同异常/RTTI设置；1 个前台线程，各自后台实现保留 |
| 写入同步 | 单次 Put，未逐条同步设备；最终 Flush/自然压缩另外计时 |
| 读取 | 每负载重新打开引擎并新建显式缓存，先执行相同负载预热一遍；操作系统页缓存未清除 |
| 数据块 CRC | 两边正常 Get 均不启用数据块 CRC 验证；MapleDB 可选 filter/MetaIndex 加载仍验证 CRC |

两个引擎共用 [compare_benchmark.cpp](../../../benchmarks/compare_benchmark.cpp) 中的计时循环。数据生成位于计时外；点查计时内校验完整 value；不存在键校验 NotFound；扫描校验每条 key/value、顺序、数量与最终状态。写入完成后的全量校验位于写入计时外。

MapleDB 通过 `Flush()` 等待最终落盘与自然后台任务；LevelDB 1.23 使用内部 `TEST_CompactMemTable()`，再通过 EnvWrapper 等待排队/执行中的任务清空。没有强制全库 CompactRange。两边保留自然压缩、输出层选择、同步与目录更新细节；读阶段也保留引擎自身可能触发的后台行为，文件布局不会被强制固定。

每轮两种配置交替：第 1/3/5 轮先关闭再开启，第 2/4 轮先开启再关闭。每配置内按轮次和数据规模交替引擎顺序，合计各有 5 次 MapleDB 优先、5 次 LevelDB 优先。两个配置各 140 行，共 **280 行**；每配置 **70 对引擎结果**、跨配置 **140 组同引擎结果**的数据 hash、请求 hash、操作数和 checksum 验证通过。

每 257 次操作抽样一次延迟，100 万条约 3892 个样本；P50/P95/P99 为各轮抽样分位数的中位数。取样不等于全量延迟分布。本次未删除慢轮或选取最佳轮，全部结果保留。

### 文件读取后端与内存限制

固定的 LevelDB 1.23 默认 POSIX Env 在 64 位环境下尝试 mmap；成功映射的未压缩 DataBlock 可直接使用映射，跳过显式 BlockCache。MapleDB 使用 pread，miss 时分配并读取块，hit 时复用缓存 Block。这些路径由本地固定版本源码确认，见 LevelDB 的 [env_posix.cc](https://github.com/google/leveldb/blob/1.23/util/env_posix.cc)、[format.cc](https://github.com/google/leveldb/blob/1.23/table/format.cc) 和 [table.cc](https://github.com/google/leveldb/blob/1.23/table/table.cc)。本次没有跟踪每个文件的 mmap 成功情况。

所以相同的 8 MiB 缓存配额不等于相同总驻留内存，两个库的 index/filter 内存也不计入该数据块配额。本机 36 GiB 内存远大于约 111 MiB 逻辑数据；这里比较的是本机预热后的完整读路径，不能据此判断冷盘 I/O、大于内存规模或相同 RSS 限制下的表现。

## 4. 环境与原始证据

| 项目 | 本次记录 |
|---|---|
| 机器 | Apple M3 Pro，12 个逻辑 CPU，36 GiB 内存，arm64 |
| 系统 / 编译器 | macOS 27.0（26A428）；Apple clang 21.0.0 |
| MapleDB | 基础提交 3e56981fda5c849bd78d26356a61ecfd5a68d559，加工作区修复及 Bloom 接入 |
| LevelDB | tag 1.23；commit 99b3c03b3284f5886f9ef9a4ef703d57373e61be |
| 编译源码 SHA256 | 509431cf793d9bbc7f683c7471ca476dc0fce84efe7ac94d2fbbb81a512c7804 |

编译源码 hash 在运行前记录、运行后复核一致，覆盖 CMakeLists、benchmark 和 include/src 的 C++ 文件，规则见 [environment.json](environment.json)。实际命令、时间戳、独立临时数据库目录见 [execution.json](execution.json)。

- 原始数据：[关闭 CSV](results-off.csv)、[开启 CSV](results-on.csv)；每次调用的 CSV 保存在 [parts](parts)。
- 正式运行所有读取、NotFound 和扫描验证通过，完整输出见 [run-output.txt](run-output.txt)。
- 正式测试前，交替运行脚本已用 1000/2000 条、2 轮完成冒烟验证。
- Bloom 接入版本此前已通过 Debug/Release 各 16/16 测试，以及 Bloom/存储回归的 ASan/UBSan；详见 [接入与验证说明](../../bloom-filter.md)。本次对 C++ benchmark 的修改仅为分批轮次编号及交替顺序，存储实现未再修改。

此前的 [无过滤器历史报告](../comparison.md) 保留原始数据和版本 hash。它可以展示上一阶段结果，本次 Bloom 的效果应使用本目录内的同版本开关对照。

## 5. 复现

准备固定版本 LevelDB 源码，验证上述 commit，在 MapleDB 根目录执行：

```bash
cmake -S . -B build-compare -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DMAPLEDB_LEVELDB_SOURCE_DIR=/tmp/leveldb-1.23
cmake --build build-compare -j4
ctest --test-dir build-compare --output-on-failure -j4

python3 benchmarks/run_bloom_comparison.py \
  --output-dir=docs/performance/bloom-reproduction \
  --sizes=100000,1000000 --rounds=5 --value-bytes=100 \
  --cache-bytes=8388608 --seed=42 --bloom-bits=10
```

输出目录必须不存在或为空。脚本创建独立临时 DB 目录，保存逐次命令、原始 CSV，并生成两边对比与开关效果汇总；成功后清理自己创建的数据库，失败时保留以供诊断。测试和 benchmark 按上述顺序运行，测量时避免并行编译或测试。旧日常测试使用固定临时路径，Debug 和 Release 两套 CTest 应串行执行。

已有 CSV 可单独重新汇总：

```bash
python3 benchmarks/summarize.py docs/performance/bloom/results-off.csv \
  docs/performance/bloom/summary-off.md
python3 benchmarks/summarize.py docs/performance/bloom/results-on.csv \
  docs/performance/bloom/summary-on.md
python3 benchmarks/summarize_bloom.py \
  docs/performance/bloom/results-off.csv docs/performance/bloom/results-on.csv \
  docs/performance/bloom/effects.md
```

下一步优先定位 LevelDB 此负载的随机命中下降，以及 MapleDB 的随机写和扫描差距；增加缓存 hit/miss、pread 字节、页错误及后台压缩时间统计，再测更大数据、冷读、并发与更新/删除混合负载。
