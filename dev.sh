#!/usr/bin/env bash
set -euo pipefail

# FlatBook development tasks.
#
# Usage:
#   ./dev.sh setup              # create .venv + install matplotlib (one-time)
#   ./dev.sh test               # Release build + unit tests
#   ./dev.sh sanitize           # Debug + ASan/UBSan + tests
#   ./dev.sh tsan               # Debug + ThreadSanitizer + tests
#   ./dev.sh fuzz [seconds]     # libFuzzer build + short fuzz run (Clang)
#   ./dev.sh tidy               # clang-tidy over the library headers (Clang)
#   ./dev.sh bench [--no-build] [--refs] # build + run benchmarks + plot docs/*.png
#   ./dev.sh tail               # per-op latency percentiles (p50..p99.9/max)
#   ./dev.sh all                # test + sanitize + bench
#
# Env:
#   BUILD_DIR   Release build dir        (default: <repo>/build)
#   ASAN_DIR    sanitizer build dir      (default: <repo>/build-asan)
#   TSAN_DIR    tsan build dir           (default: <repo>/build-tsan)
#   PYTHON      python with matplotlib   (default: <repo>/.venv/bin/python)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
ASAN_DIR="${ASAN_DIR:-$ROOT/build-asan}"
TSAN_DIR="${TSAN_DIR:-$ROOT/build-tsan}"
PYTHON="${PYTHON:-$ROOT/.venv/bin/python}"
[[ -x "$PYTHON" ]] || PYTHON="python3"
BIN="$BUILD_DIR/benchmarks/flatbook_bench"
PLOT="$ROOT/benchmarks/plot/plot_scaling.py"
OUT_DIR="$ROOT/docs"

usage() {
  sed -n '4,19p' "$0" | sed 's/^# \{0,1\}//'
}

do_setup() {
  if [[ ! -x "$ROOT/.venv/bin/python" ]]; then
    echo "== creating venv =="
    python3 -m venv "$ROOT/.venv"
  fi
  echo "== installing matplotlib =="
  "$ROOT/.venv/bin/python" -m pip install --quiet matplotlib
  echo "done: $ROOT/.venv (matplotlib ready; ./dev.sh bench will use it)"
}

do_test() {
  echo "== configure (Release) =="
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DFLATBOOK_BUILD_TESTS=ON \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wconversion"
  echo "== build =="
  cmake --build "$BUILD_DIR" -j
  echo "== test =="
  "$BUILD_DIR/tests/flatbook_tests"
}

do_sanitize() {
  echo "== configure (Debug + ASan/UBSan) =="
  cmake -S "$ROOT" -B "$ASAN_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFLATBOOK_BUILD_TESTS=ON \
    -DFLATBOOK_BUILD_BENCHMARKS=OFF \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wconversion -fsanitize=address,undefined -fno-omit-frame-pointer"
  echo "== build =="
  cmake --build "$ASAN_DIR" -j
  echo "== test =="
  ASAN_OPTIONS=detect_leaks=1 \
  UBSAN_OPTIONS=print_stacktrace=1 \
    "$ASAN_DIR/tests/flatbook_tests"
}

do_tsan() {
  echo "== configure (Debug + ThreadSanitizer) =="
  cmake -S "$ROOT" -B "$TSAN_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFLATBOOK_BUILD_TESTS=ON \
    -DFLATBOOK_BUILD_BENCHMARKS=OFF \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wconversion -fsanitize=thread -fno-omit-frame-pointer"
  echo "== build =="
  cmake --build "$TSAN_DIR" -j
  echo "== test =="
  "$TSAN_DIR/tests/flatbook_tests"
}

do_fuzz() {
  local seconds="${1:-30}"
  local fuzz_dir="${FUZZ_DIR:-$ROOT/build-fuzz}"
  echo "== configure (Clang + libFuzzer) =="
  cmake -S "$ROOT" -B "$fuzz_dir" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFLATBOOK_BUILD_FUZZER=ON \
    -DFLATBOOK_BUILD_TESTS=OFF \
    -DFLATBOOK_BUILD_BENCHMARKS=OFF \
    -DFLATBOOK_BUILD_EXAMPLES=OFF
  echo "== build =="
  cmake --build "$fuzz_dir" --target flatbook_fuzz -j
  echo "== fuzz ($seconds s) =="
  "$fuzz_dir/fuzz/flatbook_fuzz" -max_total_time="$seconds" -timeout=2
}

do_tidy() {
  echo "== clang-tidy =="
  clang-tidy --header-filter='.*/include/flatbook/.*' --quiet \
    "$ROOT/tools/all_headers.cpp" -- -std=c++20 -I"$ROOT/include"
}

do_headers() {
  echo "== header isolation =="
  local ok=1
  local h
  for h in "$ROOT"/include/flatbook/*.hpp; do
    local b
    b="$(basename "$h")"
    if ! echo "#include <flatbook/$b>" | c++ -std=c++20 -I"$ROOT/include" -fsyntax-only -x c++ - 2>/dev/null; then
      echo "FAIL: $b is not self-contained"
      ok=0
    fi
  done
  [[ $ok -eq 1 ]] && echo "all headers are self-contained"
}

do_bench() {
  local no_build=0 refs=0 report=0
  for arg in "$@"; do
    case "$arg" in
      --no-build) no_build=1 ;;
      --refs) refs=1 ;;
      --report) report=1 ;;
    esac
  done

  if [[ $no_build -eq 0 ]]; then
    echo "== configure =="
    cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    echo "== build =="
    cmake --build "$BUILD_DIR" -j
  fi

  if [[ ! -x "$BIN" ]]; then
    echo "error: benchmark binary not found at $BIN (drop --no-build)" >&2
    exit 1
  fi

  # Default: FlatBook only. --refs adds the std::map and CppTrader baselines.
  local filter="BM_Latency|BM_RestLatency|BM_Cancel|BM_OrderType"
  if [[ $refs -eq 1 ]]; then
    filter="BM_Latency|BM_MapLatency|BM_RestLatency|BM_MapRestLatency|BM_CppTraderRestLatency|BM_Cancel|BM_MapCancel|BM_OrderType"
  fi

  echo "== benchmarks =="
  "$BIN" \
    --benchmark_filter="$filter" \
    --benchmark_min_time=0.2s \
    --benchmark_repetitions=10 \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=console \
    --benchmark_out="${TMPDIR:-/tmp}/flatbook_bench.json" \
    --benchmark_out_format=json

  echo "== plot =="
  mkdir -p "$OUT_DIR"
  "$PYTHON" "$PLOT" "${TMPDIR:-/tmp}/flatbook_bench.json" "$OUT_DIR"

  if [[ $report -eq 1 ]]; then
    echo "== report =="
    "$PYTHON" "$ROOT/benchmarks/plot/make_report.py" \
      "${TMPDIR:-/tmp}/flatbook_bench.json" "$ROOT/docs/BENCHMARKS.md"
  fi

  if [[ $refs -eq 1 ]]; then
    echo "done: $OUT_DIR/{add,rest,cancel,cpptrader}_vs_price_levels.png, $OUT_DIR/order_type.png"
  else
    echo "done: $OUT_DIR/{add,rest,cancel}_vs_price_levels.png, $OUT_DIR/order_type.png"
  fi
}

do_tail() {
  local bin="$BUILD_DIR/benchmarks/flatbook_tail"
  if [[ ! -x "$bin" ]]; then
    echo "== configure =="
    cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    echo "== build =="
    cmake --build "$BUILD_DIR" --target flatbook_tail -j
  fi
  echo "== tail =="
  "$bin"
}

cmd="${1:-help}"
shift || true

case "$cmd" in
  setup)     do_setup ;;
  test)      do_test ;;
  tail)      do_tail ;;
  sanitize)  do_sanitize ;;
  tsan)      do_tsan ;;
  fuzz)      do_fuzz "$@" ;;
  tidy)      do_tidy ;;
  headers)   do_headers ;;
  bench)     do_bench "$@" ;;
  all)       do_test; do_sanitize; do_bench ;;
  help|-h|--help) usage ;;
  *)         usage; exit 1 ;;
esac
