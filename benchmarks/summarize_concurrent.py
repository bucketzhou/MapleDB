#!/usr/bin/env python3
"""Validate paired concurrent results before reporting medians and scaling."""
import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import median

WORKLOADS = (
    "write_sequential", "write_random", "get_sequential", "get_random",
    "get_hot", "get_missing", "range", "mixed_90r10w", "mixed_50r50w",
)
LABELS = dict(zip(WORKLOADS, (
    "顺序写", "随机写", "顺序 Get", "随机命中 Get", "热点 Get", "不存在键 Get",
    "短范围 Seek + Next", "90% 读 / 10% 更新", "50% 读 / 50% 更新",
)))
PAIRED = (
    "ops", "read_ops", "write_ops", "items", "range_length", "dataset_hash", "update_hash",
    "request_hash", "checksum", "final_hash", "key_bytes", "value_bytes", "cache_bytes", "seed",
    "bloom_bits_per_key", "sample_stride",
)


def validate(rows):
    if not rows:
        raise ValueError("empty concurrent CSV")
    indexed = {}
    invariants = {}
    sizes, threads, rounds = set(), set(), set()
    for row in rows:
        engine, workload = row["engine"], row["workload"]
        n, t, r = (int(row[field]) for field in ("num_keys", "threads", "round"))
        if engine not in ("MapleDB", "LevelDB") or workload not in WORKLOADS or min(n, t, r) <= 0:
            raise ValueError("invalid engine, workload or dimensions")
        sizes.add(n); threads.add(t); rounds.add(r)
        key = (n, t, r, workload, engine)
        if key in indexed:
            raise ValueError(f"duplicate result: {key}")
        indexed[key] = row
        ops, reads, writes, items, length = (int(row[field]) for field in
                                            ("ops", "read_ops", "write_ops", "items", "range_length"))
        if workload == "range":
            if not 1 <= length <= n:
                raise ValueError("invalid range length")
            expected_ops = max(1, n // length)
            expected_writes = 0
            expected_items = expected_ops * length
        else:
            expected_ops, expected_items = n, n
            if workload.startswith("write_"):
                expected_writes = n
            elif workload.startswith("mixed_"):
                slots = 1 if workload == "mixed_90r10w" else 5
                expected_writes = n // 10 * slots + min(n % 10, slots)
            else:
                expected_writes = 0
            if length != 0:
                raise ValueError("non-range workload has a range length")
        if (ops, reads, writes, items) != (expected_ops, expected_ops - expected_writes, expected_writes, expected_items):
            raise ValueError(f"incorrect operation accounting: {key}")
        elapsed, settle = float(row["elapsed_ms"]), float(row["settle_ms"])
        if not math.isfinite(elapsed) or elapsed <= 0 or not math.isfinite(settle) or settle < 0:
            raise ValueError("invalid elapsed time")
        for field, expected in (("ops_per_sec", ops * 1000 / elapsed),
                                ("items_per_sec", items * 1000 / elapsed),
                                ("complete_ops_per_sec", ops * 1000 / (elapsed + settle))):
            if not math.isclose(float(row[field]), expected, rel_tol=1e-8):
                raise ValueError(f"invalid throughput: {field}")
        stride = int(row["sample_stride"])
        if stride <= 0:
            raise ValueError("invalid sample stride")
        expected_samples = sum((ops * (i + 1) // t - ops * i // t + stride - 1) // stride for i in range(t))
        if int(row["samples"]) != expected_samples:
            raise ValueError("sample count or thread partition mismatch")
        latency = [float(row[field]) for field in ("p50_ns", "p95_ns", "p99_ns")]
        if not all(math.isfinite(x) and x >= 0 for x in latency) or latency != sorted(latency):
            raise ValueError("invalid sampled latency")
        # 线程数和轮次改变时，数据、逻辑请求及最终状态也必须保持一致。
        signature = tuple(row[field] for field in PAIRED)
        invariant = (n, workload)
        if invariant in invariants and invariants[invariant] != signature:
            raise ValueError(f"request/data/final-state mismatch: {key}")
        invariants[invariant] = signature
    if rounds != set(range(1, max(rounds) + 1)):
        raise ValueError("rounds are not consecutive from 1")
    for n in sizes:
        for t in threads:
            for r in rounds:
                for workload in WORKLOADS:
                    for engine in ("MapleDB", "LevelDB"):
                        if (n, t, r, workload, engine) not in indexed:
                            raise ValueError("incomplete engine/workload/round/thread matrix")
    return sorted(sizes), sorted(threads), sorted(rounds)


def summarize(path):
    with Path(path).open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    sizes, threads, rounds = validate(rows)
    groups = defaultdict(list)
    for row in rows:
        groups[(int(row["num_keys"]), int(row["threads"]), row["workload"], row["engine"])].append(row)

    def metric(n, t, name, engine, field):
        return median(float(row[field]) for row in groups[(n, t, name, engine)])

    config = rows[0]
    lines = ["# MapleDB / LevelDB 并发性能结果", "",
             f"{len(rows)} 行、{len(rows)//2} 对引擎结果验证通过；每配置 {len(rounds)} 轮，报告中位数。",
             f"线程数：{', '.join(map(str, threads))}；key/value={config['key_bytes']}/{config['value_bytes']} B；"
             f"显式缓存 {int(config['cache_bytes'])/1048576:g} MiB；Bloom {config['bloom_bits_per_key']} bits/key；无块压缩。", "",
             "每个配置共享一个 DB 实例，各线程独立缓冲区/迭代器。线程创建和 join 在计时外，"
             "计时包含统一起跑唤醒、请求执行和内容校验；线程数变化时总请求不变。", "",
             "范围吞吐按请求计，每请求返回固定数量的条目；混合吞吐包含读和更新。"
             "延迟为每线程每 257 次请求抽样、合并样本后的分位数，再取各轮中位数，不是全量 P99。", ""]
    for n in sizes:
        lines += [f"## {n:,} 条数据", "",
                  "吞吐单位：万请求/s；倍率=LevelDB/MapleDB；加速比=本引擎该线程数/1 线程。", "",
                  "| 负载 | 线程 | MapleDB | LevelDB | 倍率 | Maple 加速比 | Level 加速比 | Maple 抽样 P99 µs | Level 抽样 P99 µs |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for name in WORKLOADS:
            for t in threads:
                m, l = [metric(n, t, name, engine, "ops_per_sec") for engine in ("MapleDB", "LevelDB")]
                speed = [metric(n, t, name, engine, "ops_per_sec") / metric(n, 1, name, engine, "ops_per_sec")
                         if 1 in threads else None for engine in ("MapleDB", "LevelDB")]
                scale = [f"{x:.2f}×" if x is not None else "—" for x in speed]
                p99 = [metric(n, t, name, engine, "p99_ns") / 1000 for engine in ("MapleDB", "LevelDB")]
                lines.append(f"| {LABELS[name]} | {t} | {m/10000:.2f} | {l/10000:.2f} | {l/m:.2f}× | {scale[0]} | {scale[1]} | {p99[0]:.2f} | {p99[1]:.2f} |")
        lines += ["", "### 含最终 Flush / 自然后台工作", "",
                  "写入和混合负载的完整吞吐单位为万请求/s；混合行的请求数包含读与更新，不能当作纯写 QPS。", "",
                  "| 负载 | 线程 | Maple 完整吞吐 | Level 完整吞吐 | Level / Maple | Maple 排空 ms | Level 排空 ms |",
                  "|---|---:|---:|---:|---:|---:|---:|"]
        for name in ("write_sequential", "write_random", "mixed_90r10w", "mixed_50r50w"):
            for t in threads:
                m, l = [metric(n, t, name, engine, "complete_ops_per_sec") for engine in ("MapleDB", "LevelDB")]
                drain = [metric(n, t, name, engine, "settle_ms") for engine in ("MapleDB", "LevelDB")]
                lines.append(f"| {LABELS[name]} | {t} | {m/10000:.2f} | {l/10000:.2f} | {l/m:.2f}× | {drain[0]:.2f} | {drain[1]:.2f} |")
        ranges = groups[(n, threads[0], "range", "MapleDB")][0]
        lines += ["", f"范围请求每次读取 {ranges['range_length']} 条，共 {ranges['ops']} 次、{ranges['items']} 条；"
                  "原始 CSV 的 items_per_sec 记录扫描条目吞吐。范围请求较少，抽样数量可能很小；"
                  "该负载 P99 仅供参考，实际样本数见 CSV 的 samples。", ""]
    lines += ["## 结果边界", "",
              "- 纯读先执行同线程数、同请求的完整预热；混合负载先做随机读预热。操作系统页缓存未清除。",
              "- 顺序写表示每线程按自己分配的连续 key 区间递增写入，跨线程实际顺序由调度决定。",
              "- 混合负载每个更新键只有一次更新，读取允许该键的完整旧值或完整新值；最终逐键及有序扫描校验，并在重开后复核。",
              "- checksum 比较逻辑请求覆盖；不强求两引擎读取到相同版本。该验证不等同于线性一致性证明。",
              "- 逐条 Put 不同步设备；后台实现各自保留，未强制全库压缩。",
              "- 两库沿用 pread / 可尝试 mmap 的不同后端，相同显式缓存配额不代表相同 RSS。",
              "- 这些结果只对应本次机器、数据和线程配置；需要更大规模、更新热点及独立多轮实验再判断扩展性。", ""]
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(summarize(args.csv))
