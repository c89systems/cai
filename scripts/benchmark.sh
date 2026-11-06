#!/usr/bin/env bash
# Opt-in measurements, not a timing gate. Preserve every raw sample under build/.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
original=("$@")
binary_dir="$root/build/x86_64-linux-musl-release"
sizes=(64 4096 32768)
arguments=()
no_build=0
explicit_dir=0
json=0
while (($#)); do
  case "$1" in
    --json) json=1; shift ;;
    --no-build) no_build=1; shift ;;
    --verify) arguments+=("$1"); shift ;;
    --binary-dir|--message-bytes|--rounds|--warmups|--bytes|--iterations|--case)
      (($# >= 2)) || { printf '%s requires a value\n' "$1" >&2; exit 2; }
      case "$1" in
        --binary-dir) binary_dir=$2; explicit_dir=1 ;;
        --message-bytes) sizes=("$2") ;;
        *) arguments+=("$1" "$2") ;;
      esac
      shift 2 ;;
    --help)
      printf '%s\n' 'Usage: make benchmark BENCH_ARGS="[OPTIONS]"' \
        'Default: build the static musl Release runner; measure 64, 4096 and 32768 byte messages.' \
        '--no-build [--binary-dir build/DIR]  Use an existing checkout-owned runner.' \
        '--json  Print raw JSONL instead of the median Markdown table.' \
        '--message-bytes N  Measure only this message size (1..32768).' \
        '--rounds N --warmups N --bytes N --iterations N --case NAME --verify' \
        'See docs/benchmarks.md for the workload contract and interpretation.'
      exit 0 ;;
    *) printf 'Unknown benchmark option: %s\n' "$1" >&2; exit 2 ;;
  esac
done
for tool in jq rg realpath sha256sum; do
  command -v "$tool" >/dev/null || { printf '%s is required\n' "$tool" >&2; exit 2; }
done
((!explicit_dir || no_build)) || { printf '%s\n' '--binary-dir requires --no-build' >&2; exit 2; }
cd "$root"
if ((!no_build)); then
  bash scripts/cpkt-toolchains.sh ensure x86_64-linux-musl >&2
  cmake --preset cai -DCAI_BUILD_BENCHMARKS=ON >&2
  cmake --build --preset cai --target cai_benchmark cai_cli >&2
fi
binary_dir=$(realpath -e "$binary_dir")
case "$binary_dir/" in "$root/build/"?*/) ;; *) printf 'Benchmark binary must be under %s/build\n' "$root" >&2; exit 2 ;; esac
[[ -x "$binary_dir/cai_benchmark" ]] || { printf 'Build cai_benchmark first in %s\n' "$binary_dir" >&2; exit 2; }
rg -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$binary_dir/CMakeCache.txt" || {
  printf 'Benchmark build does not belong to this checkout\n' >&2; exit 2;
}
mkdir -p "$root/build/benchmarks"
run_dir=$(mktemp -d "$root/build/benchmarks/run.XXXXXX")
report="$run_dir/results.jsonl"
trap 'printf "Raw benchmark report: %s\n" "$report" >&2' EXIT
invocation=''
if ((${#original[@]})); then
  printf -v invocation '%q ' "${original[@]}"
fi
revision=$(git rev-parse HEAD)
dirty=false
[[ -z $(git status --porcelain) ]] || dirty=true
read -r binary_sha256 _ < <(sha256sum "$binary_dir/cai_benchmark")
jq -cn --arg revision "$revision" --arg command "scripts/benchmark.sh $invocation" \
  --arg binary_sha256 "$binary_sha256" --argjson dirty "$dirty" \
  '{kind:"run",revision:$revision,dirty:$dirty,command:$command,binary_sha256:$binary_sha256}' >"$report"
for size in "${sizes[@]}"; do
  "$binary_dir/cai_benchmark" "${arguments[@]}" --message-bytes "$size" >>"$report"
done
if ((json)); then
  cat "$report"
else
  jq -rs '
    def median: sort | length as $n | if $n % 2 == 1 then .[$n/2|floor]
      else (.[($n/2)-1] + .[$n/2])/2 end;
    def rounded: . * 1000 | round / 1000;
    "Medians of measured rounds; no timing acceptance threshold.",
    "| Case | Message bytes | MiB/s | Wall ms | CPU % (both processes) | RTT p50 µs | RTT p95 µs |",
    "|---|---:|---:|---:|---:|---:|---:|",
    ([.[] | select(.kind == "sample")] | group_by([.case,.message_bytes])[] |
      "| \(.[0].case) | \(.[0].message_bytes) | \(map(.mib_s)|median|rounded) | \(map(.wall_s*1000)|median|rounded) | \(map(.cpu_pct)|median|rounded) | \(map(.rtt_p50_us)|median|rounded) | \(map(.rtt_p95_us)|median|rounded) |")
  ' "$report"
fi
