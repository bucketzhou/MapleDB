# MapleDB

参考 LevelDB 设计、使用 C++17 实现的嵌入式键值存储引擎，采用 LSM-Tree 架构，支持 Put、Get、Delete、Flush 和前向有序迭代。

## 核心架构

- **写入**：WAL → SkipList MemTable → Immutable MemTable → 后台 Flush → SSTable；通过分层 Compaction 合并文件、清理旧版本和删除标记。
- **读取**：查询 MemTable、Immutable MemTable，再查询 L0 和更深层 SSTable；L0 优先检查新文件，L1+ 使用文件范围二分定位，Bloom Filter 在读取 DataBlock 前排除不存在的 key。
- **存储格式**：InternalKey 编码序列号与操作类型；SSTable 使用 DataBlock、IndexBlock、键前缀编码、Restart Point 和 Varint。
- **缓存与并发**：16 分片 LRU、TableCache、BlockCache、引用计数 handle；数据块使用 pread，SkipList 在外部串行写入的前提下支持无锁读取。
- **版本与迭代**：不可变文件元数据快照、旧 SST 保留、多路合并、L1+ 按需连接；用户迭代器按创建时序列号过滤版本与删除标记。
- **恢复**：MANIFEST/CURRENT 恢复文件集合、WAL 重放恢复未刷盘数据，支持连续重开校验。

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

当前记录：Debug 和 Release 各 16/16 测试通过；存储和 Bloom 集成回归通过 ASan/UBSan。

## Bloom Filter

默认 `Options::bloom_bits_per_key=10`，新 SST 按 user key 生成过滤器，Flush 和 Compaction 输出均包含过滤器。设为 `0` 可关闭生成和查询使用。旧的无过滤器 SST 可继续读取。

使用方式、文件格式、兼容性与验证见 [Bloom Filter 接入说明](docs/bloom-filter.md)。

## 性能与项目介绍

- [逐项本地提交与源码修复审阅清单](docs/review.md)
- [简历项目描述、关键技术和面试讲解](docs/resume.md)
- [启用 Bloom 后与 LevelDB 1.23 的性能对比](docs/performance/bloom/comparison.md)
- [Bloom 开关收益与完整波动范围](docs/performance/bloom/effects.md)
- [接入前的无过滤器基线](docs/performance/comparison.md)
- [交替运行 Bloom 开关对照](benchmarks/run_bloom_comparison.py)
- [统一 benchmark 与复现方式](benchmarks/run_comparison.sh)

最新实验对相同的 10 万/100 万条数据、相同请求序列，交替运行 Bloom 关闭/10 bits/key 两种配置，各 5 轮。百万条、单前台线程、16 B key + 100 B value、8 MiB 显式缓存、无块压缩、预热读取条件下，开启 Bloom 的 MapleDB 顺序点查约 174 万次/秒，不存在键点查约 255 万次/秒，后者比同版本关闭时提升 48.8%；两边同时开启时 LevelDB 不存在键点查约 235 万次/秒。写入和扫描仍有差距，具体条件及 LevelDB mmap 与 MapleDB pread 的差异见性能文档。统一 benchmark 默认关闭过滤器，追加 `--bloom-bits=10` 可在两边同时启用。

当前 DB 接口尚未接入块压缩、公开 Snapshot API、事务或逐次同步写选项。
