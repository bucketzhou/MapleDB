#!/usr/bin/env python3
"""Validate identical off/on workloads and summarize the effect of Bloom (stdlib only)."""
import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

NAMES = {
    "write_sequential": "顺序写入，Put 返回", "write_random": "随机写入，Put 返回",
    "get_sequential": "顺序点查", "get_random": "随机点查",
    "get_hot": "热点点查", "get_missing": "不存在键点查", "scan": "全量扫描",
}


def read(path):
    with path.open(newline="") as file:
        rows = list(csv.DictReader(file))
    if not rows:
        raise ValueError("empty CSV")
    keyed = {}
    for row in rows:
        row.setdefault("bloom_bits_per_key", "0")
        key = (row["engine"], int(row["num_keys"]), int(row["round"]), row["workload"])
        if key in keyed:
            raise ValueError(f"duplicate result: {key}")
        keyed[key] = row
    return keyed


def summarize(off_path, on_path):
    off, on = read(off_path), read(on_path)
    if off.keys() != on.keys():
        raise ValueError("off/on run sets differ")
    fields = ("num_keys", "round", "key_bytes", "value_bytes", "cache_bytes", "seed", "dataset_hash",
              "request_hash", "workload", "ops", "sample_stride", "checksum")
    bits = {r["bloom_bits_per_key"] for r in on.values()}
    if len(bits) != 1 or int(next(iter(bits))) <= 0:
        raise ValueError("on CSV must use one positive Bloom setting")
    for key in off:
        if off[key]["bloom_bits_per_key"] != "0":
            raise ValueError("off CSV has Bloom enabled")
        for field in fields:
            if off[key][field] != on[key][field]:
                raise ValueError(f"off/on {field} differs: {key}")
    groups = defaultdict(list)
    for mode, rows in ((0, off), (1, on)):
        for (engine, n, round_number, workload), row in rows.items():
            groups[(mode, engine, n, workload)].append(row)
    sizes = sorted({key[1] for key in off})

    def rates(mode, engine, n, workload, settled=False):
        return [float(r["ops"]) * 1000 / (float(r["elapsed_ms"]) +
                (float(r["settle_ms"]) if settled else 0))
                for r in groups[(mode, engine, n, workload)]]

    lines = ["# 同版本 Bloom 开关对照", "",
             f"已验证 {len(off)} 组同引擎、同规模、同轮次、同负载的开关结果：数据与请求 hash、操作数、结果 checksum 一致。",
             "", f"启用配置为 {next(iter(bits))} bits/key。倍率 = 启用中位吞吐量 / 关闭中位吞吐量，变化百分比同样基于吞吐量。",
             "每个 CSV 中两引擎的配对验证和完整逐轮范围见对应 summary。", ""]
    for n in sizes:
        lines += [f"## {n:,} 条", ""]
        for engine in ("MapleDB", "LevelDB"):
            lines += [f"### {engine}", "", "| 负载 | 关闭 ops/s 中位数（范围） | 启用 ops/s 中位数（范围） | 启用 / 关闭 | 吞吐量变化 |",
                      "|---|---:|---:|---:|---:|"]
            for workload, name in NAMES.items():
                a, b = rates(0, engine, n, workload), rates(1, engine, n, workload)
                ratio = statistics.median(b) / statistics.median(a)
                lines.append(f"| {name} | {statistics.median(a):,.0f} ({min(a):,.0f}–{max(a):,.0f}) | "
                             f"{statistics.median(b):,.0f} ({min(b):,.0f}–{max(b):,.0f}) | {ratio:.2f}× | {(ratio-1)*100:+.1f}% |")
            lines += ["", "#### 写入并等待 Flush/自然压缩完成", "",
                      "| 负载 | 关闭 ops/s | 启用 ops/s | 启用 / 关闭 | 吞吐量变化 |", "|---|---:|---:|---:|---:|"]
            for workload in ("write_sequential", "write_random"):
                a, b = [statistics.median(rates(mode, engine, n, workload, True)) for mode in (0, 1)]
                lines.append(f"| {NAMES[workload].split('，')[0]} | {a:,.0f} | {b:,.0f} | {b/a:.2f}× | {(b/a-1)*100:+.1f}% |")
            lines.append("")
        lines += ["### 最终 SST 大小", "", "不包含 WAL/MANIFEST；文件总大小取中位数，不能代替累计写放大或进程内存。", "",
                  "| 引擎 | 写入负载 | 关闭 MiB | 启用 MiB | 增加比例 |", "|---|---|---:|---:|---:|"]
        for engine in ("MapleDB", "LevelDB"):
            for workload in ("write_sequential", "write_random"):
                a, b = [statistics.median([float(r["sst_bytes"]) for r in groups[(mode, engine, n, workload)]]) for mode in (0, 1)]
                lines.append(f"| {engine} | {NAMES[workload].split('，')[0]} | {a/1048576:.2f} | {b/1048576:.2f} | {(b/a-1)*100:+.2f}% |")
        lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("off", type=Path)
    parser.add_argument("on", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(summarize(args.off, args.on), encoding="utf-8")
    print(f"Validated Bloom off/on data and wrote {args.output}")
