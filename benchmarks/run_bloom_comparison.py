#!/usr/bin/env python3
"""Alternate Bloom off/on by round while preserving the common benchmark's engine order."""
import argparse
import csv
import datetime
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=repo / "build-compare/compare_benchmark")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sizes", default="100000,1000000")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--value-bytes", type=int, default=100)
    parser.add_argument("--cache-bytes", type=int, default=8388608)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--bloom-bits", type=int, default=10)
    args = parser.parse_args()
    if min(args.rounds, args.value_bytes, args.bloom_bits) <= 0 or args.cache_bytes < 0:
        parser.error("rounds, value-bytes and bloom-bits must be positive; cache-bytes must be nonnegative")
    out = args.output_dir.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error("output-dir must be absent or empty; existing results are never overwritten")
    out.mkdir(parents=True, exist_ok=True)
    parts = out / "parts"
    parts.mkdir()
    db_root = Path(tempfile.mkdtemp(prefix="mapledb-bloom-ab-"))
    manifest = {
        "started_at": datetime.datetime.now().astimezone().isoformat(),
        "configuration_order": "off/on for odd rounds; on/off for even rounds",
        "engine_order": "alternated by round and dataset size by the common executable",
        "db_root": str(db_root),
        "parts": [],
    }
    manifest_path = out / "execution.json"

    def save_manifest():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    save_manifest()
    aggregates = {0: out / "results-off.csv", args.bloom_bits: out / "results-on.csv"}
    fields = None
    try:
        for round_index in range(args.rounds):
            modes = (0, args.bloom_bits) if round_index % 2 == 0 else (args.bloom_bits, 0)
            for bits in modes:
                part = parts / f"bloom-{bits}-round-{round_index + 1}.csv"
                run_root = db_root / f"bloom-{bits}-round-{round_index + 1}"
                command = [str(args.binary.resolve()), f"--sizes={args.sizes}", "--rounds=1",
                           f"--round-offset={round_index}", f"--value-bytes={args.value_bytes}",
                           f"--cache-bytes={args.cache_bytes}", f"--seed={args.seed}",
                           f"--bloom-bits={bits}", f"--db-root={run_root}", f"--output={part}"]
                record = {"round": round_index + 1, "bloom_bits_per_key": bits, "command": command,
                          "started_at": datetime.datetime.now().astimezone().isoformat()}
                manifest["parts"].append(record)
                save_manifest()
                print(f"=== round={round_index + 1}, Bloom={bits} bits/key ===", flush=True)
                subprocess.run(command, check=True)
                with part.open(newline="") as f:
                    reader = csv.DictReader(f)
                    rows = list(reader)
                    if fields is None:
                        fields = reader.fieldnames
                    if reader.fieldnames != fields:
                        raise ValueError("CSV schemas differ")
                if not rows or any(int(r["round"]) != round_index + 1 or
                                   int(r["bloom_bits_per_key"]) != bits for r in rows):
                    raise ValueError("CSV round/Bloom setting mismatch")
                target = aggregates[bits]
                new = not target.exists()
                with target.open("a", newline="") as f:
                    writer = csv.DictWriter(f, fieldnames=fields)
                    if new:
                        writer.writeheader()
                    writer.writerows(rows)
                run_root.rmdir() # executable has removed only its own successful DB subdirectories
                record["completed_at"] = datetime.datetime.now().astimezone().isoformat()
                save_manifest()
        for bits, target in aggregates.items():
            name = "summary-off.md" if bits == 0 else "summary-on.md"
            subprocess.run(["python3", str(repo / "benchmarks/summarize.py"), str(target), str(out / name)], check=True)
        subprocess.run(["python3", str(repo / "benchmarks/summarize_bloom.py"),
                        str(aggregates[0]), str(aggregates[args.bloom_bits]), str(out / "effects.md")], check=True)
        db_root.rmdir()
        manifest["completed_at"] = datetime.datetime.now().astimezone().isoformat()
        save_manifest()
        print(f"Completed paired Bloom experiment: {out}", flush=True)
    except Exception:
        print(f"Failed experiment: results={out}, DBs={db_root}", flush=True)
        raise


if __name__ == "__main__":
    main()
