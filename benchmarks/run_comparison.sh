#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: bash benchmarks/run_comparison.sh /path/to/leveldb-1.23 [output-directory]" >&2
  exit 2
fi
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
leveldb_source="$(cd "$1" && pwd)"
expected_commit="99b3c03b3284f5886f9ef9a4ef703d57373e61be"
if [[ "$(git -C "$leveldb_source" rev-parse HEAD)" != "$expected_commit" ]]; then
  echo "This adapter requires LevelDB 1.23 at $expected_commit" >&2
  exit 2
fi
output_dir="${2:-$repo_root/docs/performance/reproduction-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
if [[ -e "$output_dir/results.csv" ]]; then
  echo "Refusing to overwrite existing results.csv" >&2
  exit 2
fi
cmake -S "$repo_root" -B "$repo_root/build-compare" -DCMAKE_BUILD_TYPE=Release \
  -DMAPLEDB_LEVELDB_SOURCE_DIR="$leveldb_source"
cmake --build "$repo_root/build-compare" -j4
ctest --test-dir "$repo_root/build-compare" --output-on-failure -j4 | tee "$output_dir/tests.txt"
db_root="$(mktemp -d "${TMPDIR:-/tmp}/mapledb-compare.XXXXXX")"
# Keep a failed run's database for diagnosis. The executable removes only its own successful runs.
"$repo_root/build-compare/compare_benchmark" --sizes=100000,1000000 --value-bytes=100 \
  --cache-bytes=8388608 --rounds=5 --seed=42 --db-root="$db_root" \
  --output="$output_dir/results.csv" | tee "$output_dir/run-output.txt"
rmdir "$db_root"
python3 "$repo_root/benchmarks/summarize.py" "$output_dir/results.csv" "$output_dir/summary.md"
echo "Results: $output_dir"
