# MapleDB 与 LevelDB 性能对比

测试日期：2026-09-28。本文比较本次修复后的 MapleDB 工作区与 **LevelDB 1.23**，所有数字来自实际运行。

版本说明：本文数据记录 Bloom Filter 接入前的无过滤器基线，数字和源码 hash 保留原测量版本。当前 MapleDB 已接入 Bloom，默认 10 bits/key；统一 benchmark 默认显式关闭两边过滤器，使用 `--bloom-bits=10` 可同时启用。已完成同版本开关各 5 轮的新实验，见 [接入后的对比报告](bloom/comparison.md) 与 [开关收益](bloom/effects.md)；量化 Bloom 效果请使用新实验内的对照。接入实现见 [Bloom 说明](../bloom-filter.md)。

## 1. 结果概览

两边使用同一份预生成数据和请求序列，测试 **10 万条、100 万条**，每种规模各 **5 轮**。
每条记录为 **16 B key + 100 B value**，配置 8 MiB BlockCache，关闭压缩和 Bloom Filter。
每轮读取前先刷完剩余 MemTable，等待自然后台压缩完成。

下表为吞吐量中位数，单位 **万次/秒**。括号内倍率为 LevelDB / MapleDB，超过 1 表示 LevelDB 更快。

| 工作负载 | 10 万条 MapleDB | 10 万条 LevelDB（倍率） | 100 万条 MapleDB | 100 万条 LevelDB（倍率） |
|---|---:|---:|---:|---:|
| 顺序写入，Put 返回 | 37.36 | 40.87（1.09×） | 29.31 | 41.89（1.43×） |
| 随机写入，Put 返回 | 33.54 | 35.95（1.07×） | 19.61 | 26.45（1.35×） |
| 顺序点查 | 192.87 | 176.53（0.92×） | 172.48 | 158.11（0.92×） |
| 随机点查 | 87.46 | 118.61（1.36×） | 41.84 | 77.37（1.85×） |
| 热点点查 | 187.40 | 167.63（0.89×） | 158.94 | 153.05（0.96×） |
| 不存在键点查 | 182.01 | 171.02（0.94×） | 166.61 | 158.68（0.95×） |
| 全量迭代扫描 | 1082.50 | 1703.19（1.57×） | 986.85 | 1636.15（1.66×） |

**包含最终刷盘、后台压缩的写入成本**更能反映完成整批数据的代价：

| 规模 | 工作负载 | MapleDB 总耗时中位数 | LevelDB 总耗时中位数 | LevelDB 吞吐量 / MapleDB |
|---|---|---:|---:|---:|
| 10 万条 | 顺序写入 | 371.59 ms | 254.29 ms | 1.46× |
| 10 万条 | 随机写入 | 412.96 ms | 289.63 ms | 1.43× |
| 100 万条 | 顺序写入 | 3600.64 ms | 2401.32 ms | 1.50× |
| 100 万条 | 随机写入 | 6717.44 ms | 4013.64 ms | 1.67× |

在这组配置下，主要差距出现在**写入及后台处理、大规模随机点查、全量扫描**。预热后的顺序点查和热点点查较接近，部分场景 MapleDB 的中位数更高。热点点查在 100 万条时的范围有重叠，应谨慎解读 4% 左右的差距。

完整波动范围、抽样 P50/P95/P99 和 SST 文件统计见 [数据汇总](summary.md)，逐轮数字见 [原始 CSV](results.csv)。本次没有测量修复前的性能，因此这些结果不能量化本次修复带来的加速。

## 2. 先修复了哪些问题

| 问题 | 本次修改 | 验证方法 |
|---|---|---|
| BlockCache 命中仍复制整个块、重新分配缓冲区 | 缓存实际 `Block`，读请求和迭代器通过 handle 持有，结束后释放 | 小缓存、强制淘汰、并发读回归；全部 benchmark 读取校验 |
| 共享 `FILE*` 的 `fseek + fread` 多线程读会干扰文件位置 | 数据块读取改为 `pread`，处理短读、EINTR、块边界和压缩类型 | 6 个读线程并发查询，同时写入、Flush、Prune |
| TableCache 迭代器创建后立即释放 Table，淘汰后可能使用失效指针 | 迭代器注册 cleanup，销毁时释放 TableCache handle；串行处理 miss/open | 容量为 0 的 TableCache，主动 Evict 后继续 Seek；ASan |
| Get 每次复制文件列表；压缩后旧 SST 可能被删除，影响正在进行的读和延迟 Seek 的迭代器 | 发布不可变文件列表快照，读操作持有快照；GC 保留仍被引用的旧 SST | 创建迭代器但不 Seek，随后覆盖、删除、Flush，再验证旧视图 |
| SST 数据和索引迭代器使用错误比较器，内部版本排序可能出错 | 全程传递 InternalKeyComparator；文件排序和 L1+ 二分查找使用内部比较器 | 同一 key 的 sequence 300/200/100，Seek 到 250 必须得到 200 |
| 全局扫描提前打开 L1+ 的所有 SST，增加迭代器数量和 TableCache 压力 | L0 保留独立输入；L1+ 每层一个按需打开文件的连接迭代器 | 旧视图回归、两种规模全量扫描校验 |
| 恢复出的 WAL 尾部数据在第二次重开时可能丢失；文件号可能复用 | 恢复后先持久化尾部 MemTable，再发布新 WAL；标记已观察的 WAL 文件号 | 少量未 Flush 数据连续重开 3 次；多 WAL 数据重开 2 次 |
| MANIFEST/CURRENT 发布缺少完整同步和原子替换 | SST 完成后同步文件；MANIFEST 同步；CURRENT 临时文件同步后 rename，并同步目录 | 正常恢复回归；未做断电故障注入 |
| 后台状态和等待不够明确，benchmark 用 sleep 猜测刷盘结束 | 新增 `DB::Flush()`：排空剩余 MemTable并等待自然后台任务完成，传播错误；修复条件变量通知和后台状态管理 | 小缓冲区、大量 Flush/压缩的回归和 benchmark |
| CRC 初始化、缓存容量统计存在并发访问问题 | WAL 读写和 SST 写入的 CRC 初始化使用 `call_once`；统计在分片锁下读取 | 并发恢复/读写相关回归和 ASan/UBSan |
| Release 下 assert 中的操作被跳过，原 benchmark 计时和吞吐字节数失真 | 原 benchmark 显式检查 Status、验证扫描条数、使用 Flush 和实际字节数；旧日常测试保留断言 | Release 完整 CTest；新 benchmark 独立实现 |
| Open 对无效选项、不存在 DB、文件系统失败处理不足 | 检查尺寸、`create_if_missing`、WAL 打开结果，将文件系统异常返回为 Status | 新增 Open 错误路径回归 |

主要实现位于 [dbimpl.cpp](../../src/db/dbimpl.cpp)、[versionset.cpp](../../src/db/versionset.cpp)、[table.cpp](../../src/table/table.cpp)、[tablecache.cpp](../../src/db/tablecache.cpp)。
SST 写入 CRC 还去掉了一次重复计算和临时字符串拼接，见 [tablebuilder.cpp](../../src/table/tablebuilder.cpp)。

`daily_test_08` 原来用普通字符串模拟 SST 的内部键边界，本次改为真实编码的 InternalKey，以验证正确的比较器契约。新增的 [storage_regression_test.cpp](../../tests/storage_regression_test.cpp) 使用显式检查，Release 模式也执行校验。

## 3. 公平性与计时规则

### 数据和请求

- Key：固定 16 字节、补零的十进制字符串。实际存储 `0000000000000000`、`0000000000000002` 等偶数键。
- Value：固定 100 字节，按 seed=42 生成确定性二进制内容。数据生成和随机请求生成均在计时之外。
- 顺序写：按 key 升序，每个 key 写一次。
- 随机写：同一组 key 的无重复随机排列，每个 key 写一次。两边使用同一个预生成排列。
- 所有读工作负载：使用随机写入形成的完整数据集，每个工作负载重新打开 DB，创建新 BlockCache。
- 顺序点查：按 key 升序执行 `Get`，共 N 次。
- 随机点查：按相同的随机排列执行 `Get`，共 N 次。
- 热点点查：在前 1% 的 key 中均匀选择，允许重复，执行 N 次；两边请求序列完全相同。
- 不存在键：查找穿插在已有 key 之间的奇数键，执行 N 次，使大部分查询仍需进入 SST 查找。
- 全量扫描：从首键开始逐条 `Next`，返回 N 条；单独计时，区别于顺序执行 N 次 Get。

两种规模的逻辑 key+value 总量分别为 **11.06 MiB、110.63 MiB**。每次点查校验完整 value；扫描校验每条 key/value、排序、总条数和最终 Status；不存在键校验 NotFound。
CSV 记录数据 hash、请求顺序 hash、返回值 checksum，汇总脚本已验证 **70 对、140 行结果**一致。

### 参数和执行顺序

| 参数 | MapleDB | LevelDB |
|---|---|---|
| 构建 | Release，C++17，`-O2 -DNDEBUG` | 同左 |
| 前台线程 | 1 | 1 |
| WriteBuffer | 4 MiB | 4 MiB |
| DataBlock 目标大小 | 4 KiB | 4 KiB |
| restart interval | 16 | 16 |
| SST 目标大小 | 2 MiB | 2 MiB |
| L0 压缩触发文件数 | 4 | 4 |
| TableCache 容量 | 100 个 Table | `max_open_files=110`，扣除 10 个非 Table 配额后为 100 |
| 显式 BlockCache 配置 | 8 MiB | 8 MiB |
| 压缩 / Bloom Filter | 未启用 | 明确关闭 |
| 写入方式 | 单次 Put，WAL `fflush` | 单次 Put，`sync=false` |
| 读校验和验证 | 未启用 SST CRC 验证 | `verify_checksums=false` |

两个引擎在同一个可执行文件中运行相同的计时循环。按轮交替先运行哪个引擎，两个规模合计先运行次数相同。每个写工作负载使用新数据库。

读之前都执行**一遍完全相同工作负载的预热**，再执行一遍计时。每个读工作负载有独立的新引擎和新显式缓存；未清除操作系统页缓存。因此读结果描述本机预热后的表现，随机数据集超过显式缓存容量时仍可能产生大量 BlockCache miss。

前台写入只计 Put 循环；随后单独记录 `settle_ms`。MapleDB 使用新增 `Flush()`；LevelDB 使用固定版本内部的 `TEST_CompactMemTable()`，并通过 EnvWrapper 统计等待排队/执行中的后台任务清空。
两边均保留自然压缩策略，没有调用强制全库 `CompactRange`。写入完整耗时为 `elapsed_ms + settle_ms`，后续全量校验位于写入计时之外。

Put 均未逐条同步到设备。最终 SST 和 MANIFEST 的同步使用 macOS 的 `F_FULLFSYNC`，失败时退回 `fsync`；两边的目录与 CURRENT 更新细节仍各自保留。计时包含各引擎自身完成这些工作的开销。

点查和扫描计时包含相同的完整值校验成本。每 257 次操作抽样一次单次延迟；10 万条约 390 个样本、100 万条约 3892 个样本。P99 是抽样估计，不能用于严格的尾延迟 SLA 判断。

### mmap 与 BlockCache 的实际差异

本次比较保留 LevelDB 的默认 POSIX Env。64 位环境下它会尝试 mmap SST，成功后可以直接返回映射中的数据；未压缩块会标记为 `cachable=false`，避免再进入显式 BlockCache。
相关路径见固定版本的 [POSIX Env](https://github.com/google/leveldb/blob/1.23/util/env_posix.cc) 和 [ReadBlock](https://github.com/google/leveldb/blob/1.23/table/format.cc)。本次未通过系统调用跟踪记录每个文件是否 mmap 成功。

MapleDB 当前使用 `pread`，miss 时分配缓冲区并读入块，hit 时直接复用缓存 Block。因此 **8 MiB 配置相同不意味着总内存占用相同**，也不意味着两边每次查询都经过显式 BlockCache。
本机内存为 36 GiB，最大逻辑数据量约 111 MiB，操作系统缓存有条件保留全部数据。这里衡量的是两个引擎在该环境下的实际读路径性能。若要单独研究 BlockCache 算法，需要追加固定 pread 后端、缓存命中计数和系统调用计数的实验。

## 4. 架构差异与结果解释

| 子系统 | 修复后的 MapleDB | LevelDB 1.23 |
|---|---|---|
| 写入 | 单互斥锁下 WAL + MemTable，双 MemTable，独立后台线程；每次 Put 建立 batch 并 fflush | writer queue、batch grouping、MemTable/immutable、后台压缩；有同步写选项和写入压力控制 |
| 文件版本 | 不可变文件元数据快照，读/迭代器持有引用；GC 保留旧文件 | Version/VersionSet 引用计数、活跃版本跟踪、快照列表 |
| 点查 | MemTable → immutable → L0 新文件优先 → L1+ 二分定位；TableCache + BlockCache | 类似分层查询，支持 filter、seek compaction、成熟的 Version 管理 |
| 缓存 / 文件读 | Table 与 Block 都用 handle 管理；miss 走 pread；index 常驻 Table | TableCache + BlockCache；POSIX mmap/pread 后端，支持直接使用映射块 |
| 迭代器 | L0 合并、L1+ 每层按需连接，再过滤删除与旧版本；只有前向 API | 类似层次组织，完整前后向迭代 API，稳定的内部迭代器组合 |
| Flush / 压缩 | 本次增加显式 Flush；MemTable 先落 L0，保留当前压缩实现 | 可选择 MemTable 输出层，压缩选择、trivial move、重叠控制等策略更完整 |
| 功能 | 本文测量版本未接入 Bloom Filter、压缩、公开 snapshot、sync 写选项；当前工作区已接入 Bloom | 提供对应功能及文件锁等数据库保护机制 |

### 随机点查差距随规模扩大

10 万条的 SST 约 11 MiB，100 万条约 110 MiB。8 MiB 显式缓存与数据集的比例下降，MapleDB 随机点查由约 **87 万次/秒**降至 **42 万次/秒**；LevelDB 由约 **119 万次/秒**降至 **77 万次/秒**。
根据代码路径，缓存 miss 后的 pread、块分配与数据复制，以及 LevelDB 可使用 mmap，是合理的差异来源。没有采集 miss 数和调用栈，不能把 1.85× 差距全部归因于某一个函数。

### 顺序点查、热点点查比较接近

顺序请求会连续访问同一 DataBlock，热点集合只有约 0.11/1.11 MiB 的逻辑 key+value。MapleDB 缓存命中后的完整块复制已移除，这些访问模式更容易复用缓存。
结果显示在这些场景中 MapleDB 的中位数接近或略高于 LevelDB；这仍包含比较器、迭代器创建、缓存锁及相同的结果校验成本，不能推广到冷读或更大的热点集合。

### 写入应同时观察后台完成时间

100 万条随机写的前台循环相差 **1.35×**，计入最终刷盘和压缩后相差 **1.67×**。只报告 Put 返回速度会低估后台成本。
两边保留自身 MemTable 内存估算、落盘层选择和压缩策略，所以最终 SST 文件数并不一样；2 MiB 是目标大小，MemTable 输出也不保证严格按该大小拆分。
MapleDB 的 CRC、stdio 写入方式、同步次数和压缩实现都可能影响写入成本。本文没有测量累计磁盘写入量，SST 最终大小不能代替写放大。

### 扫描仍有明显差距

本次已减少 L1+ 扫描时同时打开的 Table 数，修复缓存和迭代器生命期，但 LevelDB 全量扫描仍快约 **1.57–1.66×**。block 切换、解析、合并迭代器以及 mmap/pread 的差异都值得后续 profile。
本测试把扫描与顺序 Get 分开报告，完整扫描在两个引擎上都远快于逐键 Get。

## 5. 测试环境与验证记录

| 项目 | 实际配置 |
|---|---|
| CPU | Apple M3 Pro，12 个逻辑 CPU |
| 内存 | 38,654,705,664 bytes，即 36 GiB |
| 系统 | macOS 27.0，build 26A428，arm64 |
| 编译器 | Apple clang 21.0.0，clang-2100.3.34.2 |
| 数据目录 | `/tmp/mapledb-compare-full-20260928`，本机 `/System/Volumes/Data` 卷 |
| MapleDB 基础提交 | `3e56981fda5c849bd78d26356a61ecfd5a68d559`，加本次未提交修复 |
| LevelDB | tag `1.23`，commit `99b3c03b3284f5886f9ef9a4ef703d57373e61be` |
| 编译源码 SHA256 | `a7fc564e5cf53ff9b061f9137ed39347067ce66e9b224c0b5d3e6080905bd20b` |

源码 hash 覆盖 CMakeLists、统一 benchmark 和所有 include/src 的 C++ 源码、头文件；计算规则见 [environment.json](environment.json)。这使未提交的修复版本也能对应到此次测量。

- Debug：15/15 CTest 通过，日志见 [tests-debug.txt](tests-debug.txt)。
- Release：15/15 CTest 通过，日志见 [tests-release.txt](tests-release.txt)。
- 新增存储回归通过 AddressSanitizer + UndefinedBehaviorSanitizer，日志见 [tests-sanitizer.txt](tests-sanitizer.txt)。macOS 当前运行时不支持 LeakSanitizer，使用 `detect_leaks=0`。
- 正式 benchmark 的所有 point read、NotFound、扫描内容和条数校验通过，日志见 [run-output.txt](run-output.txt)。
- 汇总脚本校验了两边每轮的数据、请求和结果一致。

验证范围包括正常关闭后的 WAL 恢复、并发读取与后台压缩、缓存淘汰和迭代器旧视图。此次未运行断电故障注入、全格式损坏模糊测试或多进程访问测试。当前 MapleDB 尚无 LevelDB 式 DB 文件锁；后续使用同一目录的多进程访问需要先补齐该机制。

## 6. 如何复现

统一 benchmark 位于 [compare_benchmark.cpp](../../benchmarks/compare_benchmark.cpp)，不复用原项目 benchmark 或 LevelDB db_bench 的计时逻辑。
LevelDB 使用外部源码目录，日常构建 MapleDB 不会下载或依赖 LevelDB。

```bash
git clone --branch 1.23 --recurse-submodules \
  https://github.com/google/leveldb.git /tmp/leveldb-1.23

# 在 MapleDB 根目录执行；输出目录应为新目录
bash benchmarks/run_comparison.sh /tmp/leveldb-1.23 \
  docs/performance/reproduction
```

脚本校验 LevelDB commit，构建 Release，先跑 CTest，再执行全部 5 轮并生成 CSV 和汇总文档。脚本会拒绝覆盖已有 results.csv；失败的 DB 目录会留下供诊断。

也可以手动执行本次命令：

```bash
cmake -S . -B build-compare -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DMAPLEDB_LEVELDB_SOURCE_DIR=/tmp/leveldb-1.23
cmake --build build-compare -j4
ctest --test-dir build-compare --output-on-failure -j4

./build-compare/compare_benchmark \
  --sizes=100000,1000000 --value-bytes=100 --cache-bytes=8388608 \
  --rounds=5 --seed=42 --db-root=/tmp/mapledb-new-empty-directory \
  --output=/tmp/mapledb-comparison.csv

python3 benchmarks/summarize.py /tmp/mapledb-comparison.csv \
  /tmp/mapledb-summary.md
```

DB root 必须不存在或为空。可执行文件只删除由该轮成功创建的子目录。若要研究缓存容量，可保持数据、seed、轮次和全部其他参数相同，仅修改 `--cache-bytes`，使用新的 DB root 和输出路径。

## 7. 后续优化优先级

1. 给 BlockCache 增加 hit/miss、读取字节和 pread 次数统计，定位随机点查成本；增加固定文件读取后端的对照实验。
2. Profile 扫描的 DataBlock 切换和合并迭代器，定位 1.66× 的差距。
3. 记录压缩输入/输出字节、同步次数、写阻塞时间，优化大规模随机写入的后台成本。
4. Bloom 已接入并完成 [开关对照](bloom/comparison.md)；继续补齐数据块 CRC 验证、同步写选项、文件锁和故障注入测试，并为这些功能建立各自的对比场景。

本报告适用于本机、给定数据形态、单前台线程、无压缩/过滤器、预热读的条件。冷缓存、大于内存的数据集、多线程和更新/删除混合负载需要另外测量。
