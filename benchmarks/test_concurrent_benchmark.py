#!/usr/bin/env python3
"""Exercise unequal partitions, both Bloom modes and paired-result validation."""
import argparse
import csv
import subprocess
import tempfile
from pathlib import Path

from summarize_concurrent import validate


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    binary = parser.parse_args().binary.resolve()
    with tempfile.TemporaryDirectory(prefix="mapledb-concurrent-smoke-") as directory:
        root = Path(directory)
        for bloom in (0, 10):
            output = root / f"{bloom}.csv"
            subprocess.run([str(binary), f"--db-root={root / str(bloom)}", f"--output={output}",
                            "--sizes=257,503", "--threads=1,3,8", "--rounds=1", "--range-length=32",
                            "--cache-bytes=0", f"--bloom-bits={bloom}"], check=True, timeout=90)
            with output.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            validate(rows)
            for mutation in ("missing", "final_hash", "ops"):
                bad = [row.copy() for row in rows]
                if mutation == "missing":
                    bad.pop()
                else:
                    bad[0][mutation] = str(int(bad[0][mutation]) + 1)
                try:
                    validate(bad)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f"validator accepted corrupt {mutation}")
        # 范围请求少于线程数时，空分区也必须正常起跑、完成且不产生伪样本。
        tiny = root / "tiny.csv"
        subprocess.run([str(binary), f"--db-root={root / 'tiny'}", f"--output={tiny}",
                        "--sizes=3", "--threads=8", "--rounds=1", "--range-length=64"], check=True, timeout=90)
        with tiny.open(newline="") as stream:
            validate(list(csv.DictReader(stream)))
    print("concurrent benchmark smoke and corruption checks passed")


if __name__ == "__main__":
    main()
