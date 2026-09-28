# 本地逐项修复审阅清单

## 当前分支与提交安排

本地审阅分支：`review/problem-fixes-20260928`。基础提交：`3e56981fda5c849bd78d26356a61ecfd5a68d559`。原始 `main` 保留在该基础提交。

此前工作区积累的修改按问题和依赖拆成下面 19 个提交，最后另加一个源码注释与本审阅清单提交，共 **20 个本地 commit**。每个提交说明都包含问题、修复方法和验证范围。未推送到远程。

## 按顺序审阅

| 序号 | Commit | 问题或工作 | 关键代码入口 |
|---|---|---|---|
| 01 | `f2e4be9` | CRC 初始化的普通 bool 存在数据竞争，改为 call_once | [logreader.cpp](../src/db/logreader.cpp)、[logwriter.cpp](../src/db/logwriter.cpp)、[tablebuilder.cpp](../src/table/tablebuilder.cpp) |
| 02 | `1649ede` | TotalCharge 与分片修改并发读取 usage_，增加分片锁 | [cache.cpp](../src/util/cache.cpp)：TotalCharge |
| 03 | `6d58555` | 写 SST 的 CRC 重复扫描和复制，改为连续计算 | [tablebuilder.cpp](../src/table/tablebuilder.cpp)：WriteBlock |
| 04 | `5720e3d` | 共享 FILE 的 fseek/fread 并发串位置，改为 pread 并检查边界/短读 | [table.cpp](../src/table/table.cpp)：ReadRawBlock；直接共享 Table 的六线程回归 |
| 05 | `58f59da` | BlockCache 命中仍复制整块，缓存 Block 并通过 handle 管理引用 | [table.cpp](../src/table/table.cpp)：ReadBlock、ReleaseBlock、TwoLevelIterator |
| 06 | `7e5b6a1` | TableCache 淘汰可能释放迭代器依赖的 Table，引用转交 cleanup | [iterator.h](../include/mapledb/iterator.h)、[tablecache.cpp](../src/db/tablecache.cpp)：NewIterator、FindTable |
| 07 | `2b4e89a` | 内部键使用字节排序导致文件定位/Seek 错误，统一 InternalKeyComparator | [versionset.cpp](../src/db/versionset.cpp)：ApplyEdit；[table.cpp](../src/table/table.cpp)：NewIterator；daily_test_08 |
| 08 | `f1a5e7c` | 元数据复制无法阻止旧 SST 被删除，读者持有快照且 GC 保留存活版本 | [versionset.cpp](../src/db/versionset.cpp)：PublishFiles、AddLiveFiles；[dbimpl.cpp](../src/db/dbimpl.cpp)：Get、NewIterator、RemoveObsoleteFiles |
| 09 | `ea3c9fb` | L1+ 扫描提前打开全部文件，改为每层按需连接 | [dbimpl.cpp](../src/db/dbimpl.cpp)：LevelIterator |
| 10 | `ba8cab4` | Open 未完整检查选项/目录/WAL 创建错误，失败统一返回 Status | [dbimpl.cpp](../src/db/dbimpl.cpp)：DBImpl::Open、DB::Open |
| 11 | `2253c47` | WAL fflush 错误未传播且提前推进 sequence，成功后再发布 | [logwriter.cpp](../src/db/logwriter.cpp)：Flush；[dbimpl.cpp](../src/db/dbimpl.cpp)：Write |
| 12 | `5f5dac2` | 连续重开可能忽略恢复尾部，且已存在 WAL 编号可能被复用 | [dbimpl.cpp](../src/db/dbimpl.cpp)：Open、RecoverLogFile；[versionset.cpp](../src/db/versionset.cpp)：MarkFileNumberUsed |
| 13 | `e77b61e` | 后台完成状态/唤醒/异常锁处理不明确，新增 Flush 和正确等待协议 | [dbimpl.cpp](../src/db/dbimpl.cpp)：RotateMemTable、Flush、BGWork、BackgroundCompaction、ScheduleCompaction |
| 14 | `19bb84d` | Finish 提前失败后 closed_ 阻止析构关闭 FILE | [tablebuilder.cpp](../src/table/tablebuilder.cpp)：析构函数 |
| 15 | `99a668b` | fflush 不保证持久化，CURRENT 覆盖和发布顺序存在风险 | [fileutil.h](../src/util/fileutil.h)、[versionset.cpp](../src/db/versionset.cpp)：SetCurrentFile、LogAndApply；TableBuilder::Finish |
| 16 | `60f7907` | Bloom 工具未进入 DB 路径，接入 user key 过滤器、持久化、兼容与回退 | [filter_block.cpp](../src/table/filter_block.cpp)、[tablebuilder.cpp](../src/table/tablebuilder.cpp)、[table.cpp](../src/table/table.cpp)、[bloom.cpp](../src/util/bloom.cpp) |
| 17 | `7f51d0d` | Release assert 跳过操作、旧 benchmark 计时和刷盘不准确 | [CMakeLists.txt](../CMakeLists.txt)、[benchmark.cpp](../tests/benchmark.cpp) |
| 18 | `d1a6ecc` | 增加同数据/同请求的 LevelDB 对比与 Bloom 开关实验 | [compare_benchmark.cpp](../benchmarks/compare_benchmark.cpp)、[run_bloom_comparison.py](../benchmarks/run_bloom_comparison.py) |
| 19 | `5d16812` | 保存架构说明、简历材料、历史基线及 Bloom 实测证据 | [resume.md](resume.md)、[Bloom 对比报告](performance/bloom/comparison.md) |
| 20 | 本清单所在提交 | 补充源码同步/过滤器内存约定，保存逐项验证与审阅说明 | [dbimpl.h](../src/db/dbimpl.h)、Table::ReadFilter、本文件及 [验证记录](review/validation.json) |

### 源码注释重点

新增注释放在关键逻辑旁，说明原因和约束：

- 为什么 call_once 必须保护 CRC 初始化，缓存统计为什么也要锁。
- pread 如何避免共享文件游标，块边界为什么用减法检查。
- 缓存 Block 与调用者 Block 的所有权区别，为什么先销毁块迭代器再释放 handle。
- 为什么 TableCache handle 和文件元数据快照都需要保留到迭代器销毁。
- InternalKey 的 sequence/type 为什么不能按字节排序、为什么不能参与 Bloom hash。
- 恢复尾部为何必须在发布新 WAL 前落盘，文件号 CAS 的 relaxed 与循环语义。
- Flush 的谓词如何处理虚假唤醒，running 为什么区别于 scheduled，notify_all 的必要性。
- SST、目录、MANIFEST、CURRENT 的同步与发布顺序，以及过滤器损坏时为什么回退。
- FilterBlockReader 借用字节存储，为什么不能引用加载函数的局部缓冲。

## 验证结果与依赖

拆分后的源码分别复制到独立验证目录：01–17 每一步完成 Debug 构建，并对有相关用例的步骤运行对应测试。测试随提交逐步补齐；例如容量为 0 的 TableCache 淘汰检查在 06 接入，sequence=250 应查到版本 200 的比较器检查在 07 接入。

旧文件快照的延迟 Seek、并发 Get/Flush/淘汰测试需要显式 Flush，在 13 接入完整检查。08/09 的中间版本分别进行了构建和已有迭代器测试。回归覆盖见 [storage_regression_test.cpp](../tests/storage_regression_test.cpp)、[bloom_integration_test.cpp](../tests/bloom_integration_test.cpp)。

最终复核：

- Debug：**16/16** CTest 通过。
- Release：**16/16** CTest 通过，两套测试串行执行，避免固定临时目录互相干扰。
- ASan/UBSan：存储与 Bloom 回归 **2/2** 通过，包括新增直接共享 Table 的六线程测试。macOS 使用 `ASAN_OPTIONS=detect_leaks=0`，未进行 LeakSanitizer 检查。
- 统一对比 benchmark：1000/2000 条、Bloom 0/10 各 2 轮冒烟通过，数据与请求配对校验通过。
- 原 benchmark：Release 下实际执行 10 万条，点查和扫描校验通过。
- 正式性能 CSV：280 行配对校验通过，文档链接检查通过。
- 每个提交的源码 blob 与独立构建验证的源码核对一致；原始 CSV 保留实际 CRLF 字节格式。

逐项执行时间、commit、变更文件和日志索引见 [validation.json](review/validation.json)。完整构建/测试输出见 [logs](review/logs)，最终整套测试与 benchmark 输出见 [18.txt](review/logs/18.txt)，消毒器输出见 [sanitizer.txt](review/logs/sanitizer.txt)。

注释及末尾换行整理不改变存储行为，但会改变源码指纹。因此性能报告仍保留**测量当时**的 SHA256，没有将旧数字伪装成重新测量结果。本次复核是正确性与复现冒烟检查，没有重跑正式五轮性能实验。

验证未覆盖逐项 WAL/SST I/O 故障注入、断电恢复或并发进程打开同一 DB；不能把构建和正常恢复测试解释为这些场景已经验证。

## 如何逐项查看

当前工作区已在审阅分支，可以直接查看：

```bash
# 按依赖顺序列出所有本地提交
git log --reverse --oneline main..review/problem-fixes-20260928

# 查看某项的提交说明、变更文件与源码 diff
git show --stat 5720e3d
git show 5720e3d

# 查看整组修改
git diff main..review/problem-fixes-20260928 -- src include tests CMakeLists.txt
```

这些提交按依赖顺序组织。例如 05 的缓存生命周期接口依赖 04 的块读取，09 的延迟文件打开依赖 08 的文件快照，16 的 Bloom 接入依赖比较器和持久化路径。选择部分修改时，应一起审阅所需前置提交，避免独立 cherry-pick 一个下游步骤。

审阅某个历史步骤时可在新的 worktree 检出其 commit，使用独立 build 目录构建；不要同时运行不同 worktree 的旧日常测试，因为它们使用相同的临时路径。

## 审阅后手动推送

如果决定推送这条审阅分支：

```bash
git push -u origin review/problem-fixes-20260928
```

如果决定将整组提交合入 main，再推送 main，可在工作区干净且 main 仍为本次基础提交时执行：

```bash
git switch main
git merge --ff-only review/problem-fixes-20260928
git push origin main
```

以上发布命令由你审阅后手动决定执行。
