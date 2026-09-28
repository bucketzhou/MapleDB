# Bloom Filter 接入说明

## 使用

```cpp
mapledb::Options options;
options.bloom_bits_per_key = 10; // 默认启用，约 10 bits / user key
// options.bloom_bits_per_key = 0; // 关闭生成和查询使用
```

正数启用，0 或负数关闭。已有过滤器保存了实际哈希探测次数，所以重开时把 10 改为 20 仍能读取旧过滤器；新 SST 按新参数生成。

## 写入与读取路径

1. `TableBuilder` 收集 user key，按 DataBlock 起始 offset 所在的 **2 KiB 文件区间**构建过滤器。同一区间可包含多个 DataBlock，跨区间的大块按起始位置查询。
2. InternalKey 的序列号和操作类型不参与 Bloom hash；相邻重复 user key 去重，value 和 tombstone 都加入过滤器。
3. Finish 将 Filter Block 写入 SST，再在 MetaIndex 的 `filter.mapledb.BloomFilter.v1` 中记录 key 模式和 BlockHandle。Footer 格式保持原样。
4. Table 打开时读取可选 MetaIndex/Filter Block，校验 CRC，把过滤器保留在 Table 中。过滤器与 index 一样不使用 DataBlock LRU 配额。
5. InternalGet 先从 index 找到候选数据块，再检查对应过滤器。判定不可能存在时跳过 DataBlock 读取；可能存在时继续原查找流程。
6. Flush、恢复落盘和 Compaction 共用 TableBuilder，并传递 InternalKeyComparator，因此新输出都会自动携带过滤器。

顺序迭代和范围 Seek 沿用 index/DataBlock 迭代器，不通过 Bloom 跳过数据。

## 文件格式

```text
DataBlocks
FilterBlock = [Bloom bit arrays]
              [fixed32 filter start offsets]
              [fixed32 offset-array start]
              [uint8 base_lg = 11]
MetaIndex   = "filter.mapledb.BloomFilter.v1"
              → [uint8 key mode][varint BlockHandle]
IndexBlock
Footer      = 原有的 48 字节 Footer
```

过滤器按原有物理块格式保存，包含 compression type 和 CRC32。Hash 的 4 字节输入使用固定小端解码，保证持久化行为不依赖机器字节序。

直接使用 TableBuilder 构建 InternalKey SST 时，应传第三个参数：

```cpp
mapledb::InternalKeyComparator comparator;
mapledb::TableBuilder builder(path, options, &comparator);
```

两参数 TableBuilder 默认按普通字节 key 构建，匹配默认的 Bytewise Table::Open。读取方与文件中的 key 模式不一致时会忽略过滤器。未知比较器也忽略过滤器，避免字节 hash 与自定义相等规则冲突。

## 兼容性与回退

- 旧 SST 没有对应 MetaIndex 项时继续正常查询；打开旧文件不会自动重写，后续压缩输出可生成过滤器。
- 关闭 Bloom 时，已有过滤器也不会用于查询。
- 不认识的 metadata、key 模式、偏移编码，或 MetaIndex/Filter CRC 不匹配时忽略过滤器。
- 过滤器从不提供最终的“找到”结果；可能存在仍需要查找 SST。过滤器缺失/损坏后的正确性依赖原有 index/DataBlock 路径。
- 过滤器按 user key 查询，lookup 的序列号变化不会造成漏查。删除标记加入过滤器，避免跳过删除表后读到旧值。

## 验证

[bloom_integration_test.cpp](../tests/bloom_integration_test.cpp) 验证：

- 区间映射、空区间、大块跨区间行为所需的起始 offset 映射。
- 空 key、含 NUL 的二进制 key、短 key，多版本 key 和不同查询序列号。
- 过滤器实际持久化、开关切换、读取旧无过滤器 SST。
- CRC 损坏、过滤器偏移结构损坏、未知比较器的安全回退。
- 混合新旧 SST 的覆盖、删除、Flush、Compaction、重开与全量扫描。
- 独立 L0 删除表正确遮蔽旧值。
- 固定不存在键请求下，过滤器确实减少读入的 DataBlock：1023 次请求，开启时缓存计费 2242 B，关闭时 120714 B。该数字描述此测试的不同块驻留量，不是通用性能提升比例或累计磁盘 I/O。

此外，原存储回归覆盖并发 Get、后台 Flush/压缩、缓存淘汰和旧迭代器视图，默认启用 Bloom 运行。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

Debug/Release 各 16/16 通过，Bloom 和存储回归通过 ASan/UBSan。macOS 当前 ASan 运行时使用 `detect_leaks=0`。

## 与 LevelDB 对照

统一 benchmark 默认 `--bloom-bits=0` 保留无过滤器基线；追加 `--bloom-bits=10` 为两边使用相同 bits/key。
CSV 新增 `bloom_bits_per_key`，汇总脚本验证配对设置一致，并兼容原有无过滤器 CSV。不同开关结果应写入不同文件。

```bash
./build-compare/compare_benchmark \
  --sizes=100000,1000000 --rounds=5 --seed=42 \
  --value-bytes=100 --cache-bytes=8388608 --bloom-bits=10 \
  --db-root=/tmp/mapledb-bloom-comparison-new \
  --output=/tmp/mapledb-bloom-comparison.csv
```

已完成同版本的 10 万/100 万条、Bloom 关闭与 10 bits/key 各 5 轮对照。百万条 MapleDB 不存在键查询中位吞吐从 171.61 提升至 255.35 万次/s（+48.8%）；两边同时开启时 LevelDB 为 234.54 万次/s。完整负载、波动范围、代价和读取后端差异见 [新性能报告](performance/bloom/comparison.md) 与 [开关效果](performance/bloom/effects.md)。

交替运行两种配置并生成报告：

```bash
python3 benchmarks/run_bloom_comparison.py \
  --output-dir=docs/performance/bloom-reproduction \
  --sizes=100000,1000000 --rounds=5 --bloom-bits=10
```
