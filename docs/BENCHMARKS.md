# Benchmarks

FlatBook is benchmarked across three book configurations (32K-524K orders,
1K-262K price levels) and two price distributions (uniform, clustered),
covering add, rest, cancel, per-order-type and tail latency, plus throughput.
The same workloads run through `std::map`, CppTrader and Liquibook reference
books are in [COMPARISON.md](COMPARISON.md).

Measured with Google Benchmark, GCC 15, `-O3` (add `-DFLATBOOK_NATIVE=ON` for
`-march=native`), `CMAKE_BUILD_TYPE=Release`.

## Test Environment

OS: Ubuntu 26.04.1 LTS  
CPU: AMD Ryzen 7 9800X3D 8-Core Processor (8 cores / 16 threads, 96 MB L3, 1 MB L2 per core)  
RAM: 64 GB @ 5200 MT/s  
Governor: performance, benchmark pinned to core 3  

<!-- BENCHMARKS:REPORT -->
<!-- Regenerate with: ./dev.sh bench --report -->
- CPU: (run `./dev.sh bench --report`)
- OS: (run `./dev.sh bench --report`)
- Compiler: (run `./dev.sh bench --report`)
<!-- BENCHMARKS:REPORT -->

The benchmark sweeps three book configurations and two price-generation modes
(see `benchmarks/`):

- **Uniform** - prices drawn uniformly across the full configured tick range. An
  adversarial stress pattern that exposes how the structure behaves when the
  occupied range is wide.
- **Clustered** - a synthetic mid-price random-walks slowly, each order's price a
  small gaussian offset from it, approximating real microstructure.

## Add latency

Median of 10 runs; 37% (Uniform) and 45% (Clustered) of the orders cross the book.

| BookSize / PriceLevels | Uniform | Clustered |
|---|---|---|
| 32,768 / 1,024 | 28.6 ns | 21.7 ns |
| 131,072 / 16,384 | 28.2 ns | 20.0 ns |
| 524,288 / 262,144 | 28.4 ns | 19.7 ns |

**Why it scales.** Best-bid/best-ask discovery is O(1) via the multi-level
bitmap, and insert/remove are O(1), so latency is flat across the full 256x
price range (28.6 → 28.4 ns uniform). The configured `levels_` array grows with
`PriceLevels`, but the working set touched per operation stays small.

## Tail latency

Means and medians hide the worst case, so `./dev.sh tail [core] [passes]`
measures per-operation latency percentiles with a serialized TSC
(`rdtscp` + `lfence`): 200,000 samples per pass, median of 5 passes, every
combination of book capacity (32,768 / 262,144 / 524,288 orders) and price range
(1,024 / 16,384 / 262,144 levels). Percentiles are in nanoseconds.

FlatBook, ranges across all book capacities and price ranges:

| Workload | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| add, Uniform (mixed crossing) | 20-30 | 40-50 | 80-90 | 110-130 |
| add, Clustered | 20 | 30 | 50-60 | 80 |
| rest, Uniform | 10-20 | 10-20 | 10-30 | 20-40 |
| rest, Clustered | 10 | 10 | 10 | 20 |

Book capacity has no visible effect: for the same price range and distribution
the percentiles are identical at 32K, 262K and 524K orders.

**Timer resolution.** On the test CPU (Zen 5) the TSC advances in steps of 47
cycles, i.e. one tick every 10 ns, so every per-operation value is a multiple
of ~10 ns. For `add` (50-130 ns) this is a small error; for `rest` (about 10 ns)
read the numbers as upper bounds. Use the Google Benchmark averages above for
the precise rest cost (7.4-13.9 ns), which are not affected by the timer step.

## Running the benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLATBOOK_NATIVE=ON
cmake --build build -j
./build/benchmarks/flatbook_bench                       # everything
./build/benchmarks/flatbook_bench --benchmark_filter=BM_Latency
./build/benchmarks/flatbook_bench --benchmark_filter=BM_OrderType
```

Tail-latency percentiles (pin to an idle core; default core 3, 5 passes):

```bash
./dev.sh tail            # core 3, 5 passes
./dev.sh tail 5 10       # core 5, 10 passes
```

The order-type sweep measures `addOrder` per type (`LimitGTC`, `Market`,
`LimitIOC`, `LimitFOK`, `PostOnly`). Note: on random flow, FOK orders are mostly
rejected immediately, so its number reflects the cheap reject path, not a
deep-book full fill.

The dispatcher throughput benchmark is `flatbook_dispatcher_bench`: end-to-end
~8.6M orders/s with one worker (mixed crossing, 256 symbols), ~1.85× at 2
workers and then flat (~1.98× at 8) - the single ingress and collector threads
bound how far it scales, so beyond ~2 workers matching is no longer the
bottleneck. Threads are pinned and put on SCHED_FIFO via
`benchmarks/dispatcher.conf` (ingress → core 2, collector → core 3, workers →
core 4+); run as root or with `CAP_SYS_NICE` for the real-time part.

## Graphs

Latency per order type:

![order types](order_type.png)

One command to build, run and re-plot everything (requires `matplotlib`):

```bash
PYTHON=/path/to/python-with-matplotlib ./dev.sh bench
```

Or manually:

```bash
./build/benchmarks/flatbook_bench \
    --benchmark_filter="BM_Latency|BM_MapLatency|BM_RestLatency|BM_MapRestLatency|BM_CppTraderRestLatency|BM_Cancel|BM_MapCancel|BM_OrderType" \
    --benchmark_min_time=0.2s --benchmark_repetitions=10 --benchmark_report_aggregates_only=true \
    --benchmark_format=json --benchmark_out=/tmp/bench.json

python3 benchmarks/plot/plot_scaling.py /tmp/bench.json docs
```

Writes `docs/add_vs_price_levels.png`, `docs/rest_vs_price_levels.png`,
`docs/cancel_vs_price_levels.png`, `docs/cpptrader_vs_price_levels.png` and
`docs/order_type.png`. The reference-vs-FlatBook charts are in
[COMPARISON.md](COMPARISON.md).

## Limitations of these numbers

- **Synthetic flow.** Prices are drawn from a uniform or clustered distribution,
  not a real day's LOBSTER/ITCH stream; the adversarial wide-range pattern is
  intentionally hard for flat arrays, the clustered one intentionally easy.
- **"Flat latency" is about capacity, not depth.** The sweep varies the
  *configured* capacity (32K-524K orders), but the working set stays small: the
  warm workload crosses ~37-45% immediately and resets the book periodically, so
  the actual resting depth is far below capacity. "Capacity has no effect on
  percentiles" is therefore true by construction; a genuinely deep book
  (hundreds of thousands of live orders) is a separate, not-yet-run scenario.
- **Single thread, single core.** The latency numbers are one thread on one
  core; they say nothing about multi-symbol throughput or contention.
- **Frequency and boost.** `./dev.sh tail` warns when the governor is not
  `performance` or Intel turbo is enabled; it does not detect AMD boost, so check
  it manually.
- **Timer resolution.** The TSC on the test CPU ticks every 10 ns (47 cycles), so
  per-operation percentiles are multiples of 10 ns and values around 10 ns
  (rest) are upper bounds. Treat differences below ~10 ns in the tail tables as
  noise; use the Google Benchmark averages for the exact rest cost.
- **Baselines are containers, not full matchers.** `std::map` and CppTrader
  baselines maintain levels but do not match (no crossing, no order types, no
  STP); the comparison is on the resting-only path, which is where their data
  structures differ. The `std::map` baseline uses a `std::deque` FIFO per level
  (a block allocation per level); `std::unordered_map` (HashMapBook) is a fairer
  hash-based reference, and FlatBook beats both for the same reason - the flat
  array + bitmap avoids per-level allocation and the sorted-price index. See
  [COMPARISON.md](COMPARISON.md) for real matchers.
- **CI is noisy.** Regenerate on a quiet, dedicated machine before publishing.
- **Large L3 cache.** The test CPU has 96 MB of L3 (3D V-Cache), so most of the
  configurations here are largely cache-resident. On CPUs with a smaller L3 the
  wide-range Uniform cases (262,144 levels) will likely be slower; the Clustered
  cases touch a small working set and should be less affected.
- **Single machine.** All numbers come from one workstation; results on other
  CPUs and under virtualization will differ.
