#!/usr/bin/env python3
"""Run concurrent comparisons in isolated directories and preserve evidence."""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from summarize_concurrent import summarize


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build-compare/concurrent_benchmark"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sizes", default="100000")
    parser.add_argument("--threads", default="1,2,4,8")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--range-length", type=int, default=64)
    parser.add_argument("--value-bytes", type=int, default=100)
    parser.add_argument("--cache-bytes", type=int, default=8388608)
    parser.add_argument("--bloom-bits", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[1]
    sources = [repo / "CMakeLists.txt", *sorted((repo / "src").rglob("*.cpp")),
               *sorted((repo / "src").rglob("*.h")), *sorted((repo / "include").rglob("*.h")),
               *sorted((repo / "benchmarks").glob("*.cpp")), *sorted((repo / "benchmarks").glob("*.h"))]
    digest = hashlib.sha256()
    for source in sources:
        digest.update(str(source.relative_to(repo)).encode() + b"\0" + source.read_bytes() + b"\0")
    root = Path(tempfile.mkdtemp(prefix="mapledb-concurrent-compare-"))
    command = [str(binary), f"--db-root={root}", f"--output={output / 'results.csv'}"]
    command += [f"--{name.replace('_', '-')}={getattr(args, name)}" for name in
                ("sizes", "threads", "rounds", "range_length", "value_bytes", "cache_bytes", "bloom_bits", "seed")]
    metadata = dict(started_at=datetime.now(timezone.utc).isoformat(), command=command,
                    platform=platform.platform(), architecture=platform.machine(),
                    logical_cpus=os.cpu_count(), source_sha256=digest.hexdigest(),
                    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    parameters=vars(args) | {"binary": str(binary), "output_dir": str(output)},
                    db_root=str(root), status="running")
    cache = binary.parent / "CMakeCache.txt"
    if cache.exists():
        fields = {}
        for line in cache.read_text().splitlines():
            if "=" in line and not line.startswith(("#", "//")):
                key, value = line.split("=", 1)
                fields[key.split(":", 1)[0]] = value
        metadata["cmake_cache"] = {key: fields.get(key) for key in
                             ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS_RELEASE",
                              "MAPLEDB_LEVELDB_SOURCE_DIR")}
        # 顶层 CMake 的普通变量可能覆盖 cache；flags.make 才是 Makefile 构建的实际选项。
        metadata["compile_flags"] = {}
        for target, relative in {
            "mapledb": "CMakeFiles/mapledb.dir/flags.make",
            "leveldb": "leveldb/CMakeFiles/leveldb.dir/flags.make",
            "concurrent_benchmark": "CMakeFiles/concurrent_benchmark.dir/flags.make",
        }.items():
            flags = binary.parent / relative
            if flags.exists():
                metadata["compile_flags"][target] = next(
                    (line.split("=", 1)[1].strip() for line in flags.read_text().splitlines()
                     if line.startswith("CXX_FLAGS =")), "unknown")
        level_source = fields.get("MAPLEDB_LEVELDB_SOURCE_DIR")
        if level_source:
            revision = subprocess.run(["git", "-C", level_source, "rev-parse", "HEAD"],
                                      capture_output=True, text=True)
            metadata["leveldb_commit"] = revision.stdout.strip() if revision.returncode == 0 else "unknown (no Git metadata)"
    record = output / "execution.json"
    record.write_text(json.dumps(metadata, indent=2, default=str) + "\n")
    try:
        with (output / "run-output.txt").open("w") as log:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line); log.flush()
            if process.wait() != 0:
                raise RuntimeError(f"benchmark failed; retained DB root: {root}")
        (output / "comparison.md").write_text(summarize(output / "results.csv"))
        root.rmdir()  # C++ 仅清理本次成功创建的子目录；失败时保留数据库用于诊断。
        metadata["status"] = "passed"
    except BaseException as error:
        metadata["status"] = "failed"
        metadata["error"] = str(error)
        raise
    finally:
        metadata["finished_at"] = datetime.now(timezone.utc).isoformat()
        record.write_text(json.dumps(metadata, indent=2, default=str) + "\n")


if __name__ == "__main__":
    main()
