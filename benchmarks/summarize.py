#!/usr/bin/env python3
"""Validate paired runs and generate tables from the raw comparison CSV (stdlib only)."""
import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

WORKLOADS = (
    "write_sequential", "write_random", "get_sequential", "get_random",
    "get_hot", "get_missing", "scan",
)
NAMES = {
    "write_sequential": "顺序写入", "write_random": "随机写入",
    "get_sequential": "顺序点查", "get_random": "随机点查",
    "get_hot": "热点点查", "get_missing": "不存在键点查", "scan": "全量扫描",
}


def summarize(source: Path) -> str:
    with source.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError("empty CSV")
    pairs = defaultdict(dict)
    groups = defaultdict(list)
    for row in rows:
        row.setdefault("bloom_bits_per_key", "0") # 接入前的 CSV 都为关闭过滤器的基线。
        engine = row["engine"]
        if engine not in ("MapleDB", "LevelDB"):
            raise ValueError(f"unknown engine: {engine}")
        key = (int(row["num_keys"]), int(row["round"]), row["workload"])
        if engine in pairs[key]:
            raise ValueError(f"duplicate result: {key} {engine}")
        pairs[key][engine] = row
        groups[(key[0], key[2], engine)].append(row)
    same_fields = (
        "num_keys", "key_bytes", "value_bytes", "cache_bytes", "seed", "dataset_hash",
        "request_hash", "workload", "ops", "sample_stride", "checksum",
        "bloom_bits_per_key",
    )
    for key, pair in pairs.items():
        if set(pair) != {"MapleDB", "LevelDB"}:
            raise ValueError(f"incomplete pair: {key}")
        for field in same_fields:
            if pair["MapleDB"][field] != pair["LevelDB"][field]:
                raise ValueError(f"paired {field} mismatch: {key}")
    sizes = sorted({int(row["num_keys"]) for row in rows})
    rounds = {int(row["round"]) for row in rows}
    if len(rows) != len(sizes) * len(rounds) * len(WORKLOADS) * 2:
        raise ValueError("incomplete workloads or rounds")
    for n in sizes:
        for workload in WORKLOADS:
            for engine in ("MapleDB", "LevelDB"):
                group = groups[(n, workload, engine)]
                if {int(r["round"]) for r in group} != rounds:
                    raise ValueError("unbalanced rounds")

    def values(n, workload, engine, field):
        return [float(r[field]) for r in groups[(n, workload, engine)]]

    def throughput(n, workload, engine, settled=False):
        return [float(r["ops"]) * 1000 / (float(r["elapsed_ms"]) +
                (float(r["settle_ms"]) if settled else 0))
                for r in groups[(n, workload, engine)]]

    bloom_modes = {row["bloom_bits_per_key"] for row in rows}
    if len(bloom_modes) != 1:
        raise ValueError("different Bloom configurations must use separate CSV files")
    bloom_bits = int(next(iter(bloom_modes)))
    lines = ["# 原始数据汇总", "", f"已验证 {len(pairs)} 对结果：数据、请求顺序、操作数和校验结果均一致。",
             "", f"Bloom Filter：{'关闭' if bloom_bits == 0 else str(bloom_bits) + ' bits/key（两个引擎均启用）'}。",
             "", "吞吐量取各轮中位数；范围为最小值–最大值。倍率 = LevelDB 中位吞吐量 / MapleDB 中位吞吐量。",
             "超过 1 表示 LevelDB 更快。写入主表仅计 Put 循环，完整写入成本另见下表。", ""]
    for n in sizes:
        lines += [f"## {n:,} 条", "", "### 吞吐量", "",
                  "| 工作负载 | MapleDB ops/s 中位数（范围） | LevelDB ops/s 中位数（范围） | LevelDB / MapleDB |",
                  "|---|---:|---:|---:|"]
        for workload in WORKLOADS:
            m, l = [throughput(n, workload, engine) for engine in ("MapleDB", "LevelDB")]
            lines.append(f"| {NAMES[workload]} | {statistics.median(m):,.0f} ({min(m):,.0f}–{max(m):,.0f}) | "
                         f"{statistics.median(l):,.0f} ({min(l):,.0f}–{max(l):,.0f}) | {statistics.median(l) / statistics.median(m):.2f}× |")
        lines += ["", "### 写入并等待刷盘、自然压缩完成", "",
                  "| 工作负载 | MapleDB 总耗时中位数 ms | LevelDB 总耗时中位数 ms | MapleDB ops/s | LevelDB ops/s | LevelDB / MapleDB |",
                  "|---|---:|---:|---:|---:|---:|"]
        for workload in WORKLOADS[:2]:
            m, l = [throughput(n, workload, engine, True) for engine in ("MapleDB", "LevelDB")]
            times = [statistics.median([float(r["elapsed_ms"]) + float(r["settle_ms"])
                     for r in groups[(n, workload, engine)]]) for engine in ("MapleDB", "LevelDB")]
            lines.append(f"| {NAMES[workload]} | {times[0]:,.2f} | {times[1]:,.2f} | {statistics.median(m):,.0f} | "
                         f"{statistics.median(l):,.0f} | {statistics.median(l) / statistics.median(m):.2f}× |")
        lines += ["", "### 抽样单次延迟", "", "单位 µs，各轮抽样分位数的中位数。每 257 次操作抽样一次，包含值校验；不等同于全量延迟分布。",
                  "扫描的样本是读取当前键值并 Next 的耗时，完整扫描吞吐量还包含迭代器创建。", "",
                  "| 工作负载 | MapleDB P50 / P95 / P99 | LevelDB P50 / P95 / P99 |", "|---|---:|---:|"]
        for workload in WORKLOADS:
            cells = [" / ".join(f"{statistics.median(values(n, workload, engine, field)) / 1000:.3f}"
                               for field in ("p50_ns", "p95_ns", "p99_ns"))
                     for engine in ("MapleDB", "LevelDB")]
            lines.append(f"| {NAMES[workload]} | {cells[0]} | {cells[1]} |")
        lines += ["", "### 刷盘后的 SST 文件", "", "文件数和文件总字节数取中位数；不包含 WAL/MANIFEST，也不是累计写放大。", "",
                  "| 写入方式 | 引擎 | SST 文件数 | SST 总大小 MiB |", "|---|---|---:|---:|"]
        for workload in WORKLOADS[:2]:
            for engine in ("MapleDB", "LevelDB"):
                lines.append(f"| {NAMES[workload]} | {engine} | {statistics.median(values(n, workload, engine, 'sst_files')):.0f} | "
                             f"{statistics.median(values(n, workload, engine, 'sst_bytes')) / 1048576:.2f} |")
        lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(summarize(args.csv), encoding="utf-8")
    print(f"Validated CSV and wrote {args.output}")
