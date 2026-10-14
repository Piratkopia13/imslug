#!/bin/sh
# ============================================================================
# imslug - scripts/bench.sh
#
# Benchmark harness (M4): builds imslug_tests_bench and runs it.
#
#   scripts/bench.sh              CPU pre-pass + GPU (headless) with a report
#   scripts/bench.sh --cpu        CPU-side numbers only (no Vulkan device)
#   scripts/bench.sh --gpu        GPU numbers only
#
# The markdown report is written to <build dir>/bench/bench_report.md; the
# numbers are machine-specific (record the device name from the report when
# quoting them). Budgets are not enforced (informational).
#
# Overrides: IMSLUG_BUILD_DIR (default: build), IMSLUG_BENCH_ARGS (extra
# arguments).
# ============================================================================

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$root"

build_dir=${IMSLUG_BUILD_DIR:-build}
report_dir=$build_dir/bench

"$script_dir/build.sh" --target imslug_tests_bench

bench=$build_dir/tests/imslug_tests_bench
if [ ! -x "$bench" ]; then
    echo "[bench] ERROR: $bench not found after the build." >&2
    exit 1
fi

mkdir -p "$report_dir"
report=$report_dir/bench_report.md

args=${*:-}
if [ -z "$args" ]; then
    args="--all"
fi
if [ -n "${IMSLUG_BENCH_ARGS:-}" ]; then
    args="$args $IMSLUG_BENCH_ARGS"
fi

echo "[bench] running $bench $args"
"$bench" $args --report "$report"
echo "[bench] report: $report"
