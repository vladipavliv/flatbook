# Benchmarks

FlatBook is benchmarked across three book configurations (32K-524K orders,
1K-262K price levels) and two price distributions (uniform, clustered),
covering add, rest, cancel, per-order-type and tail latency, plus throughput.
The same configurations are also run through `std::map`, CppTrader and
Liquibook reference books, comparing them apples-to-apples.

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

## std::map baseline

A `std::map`-based book (`BM_MapLatency`, same workload, with an `unordered_map`
id→level index so its cancel is not a whole-book scan) shows the contrast on the
uniform pattern:

| Price levels | FlatBook | std::map |
|---|---|---|
| 1,024 | 28.6 ns | 72.7 ns |
| 16,384 | 28.2 ns | 91.0 ns |
| 262,144 | 28.4 ns | 110 ns |

The map grows with the price range (every level lookup chases a tree node, every
insert allocates); FlatBook stays flat because its price discovery is a constant
number of bitmap levels and it allocates nothing.

The mixed-crossing workload above is the *best case* for `std::map`: roughly 40%
of the orders match and never insert. The per-workload picture (uniform, 1,024
levels):

| Workload | FlatBook | std::map | ratio |
|---|---|---|---|
| add, mixed crossing (`BM_Latency`/`BM_MapLatency`) | 28.6 ns | 72.7 ns | 2.5× |
| add, resting-only (`BM_RestLatency`/`BM_MapRestLatency`) | 9.4 ns | 71.1 ns | 7.6× |
| add/cancel mix, 25% cancel (`BM_Cancel`/`BM_MapCancel`) | 25.4 ns | 72.9 ns | 2.9× |

The resting gap widens with `PriceLevels` (from 1,024 to 262,144 levels FlatBook
goes 9.4 → 13.9 ns, `std::map` 71 → 257 ns): every rest in a map is a tree-node
`new`, FlatBook writes one flat-array slot. The map's cancel is an O(1) index
lookup plus an O(k) scan of the level queue plus an erase; FlatBook cancels by
id in O(1).

## CppTrader baseline

FlatBook is also measured against [CppTrader](https://github.com/chronoxor/CppTrader)'s
optimized market manager - the same flat-array + sorted-price-level technique,
vendored under its MIT license in `benchmarks/src/reference/cpptrader/`. It is a
market-data replay container (it maintains levels but does not match), so the
comparison is on the resting-only path only. Median of 10 runs:

| Price levels | FlatBook | std::map | CppTrader |
|---|---|---|---|
| 1,024 | 9.4 ns | 71.1 ns | 283 ns |
| 16,384 | 10.2 ns | 132 ns | 4,244 ns |
| 262,144 | 13.9 ns | 257 ns | 11,376 ns |

CppTrader stores price levels in a `std::vector` sorted by price and finds a
level by scanning linearly back from the best price, and inserting a *new* level
shifts every element after it (a memmove). A far-away level therefore pays two
O(levels) penalties - a scan plus a shift - where FlatBook pays neither: price
*is* the array index (O(1) insert), and the hierarchical bitmap finds the best
price in O(1). Near the touch (clustered) the scan is short and the insert lands
at the end (~105 ns, flat); on wide-range uniform flow both penalties bite, and
it degrades from 283 ns to 11.4 µs.

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

`std::map` rest, at 262,144 orders:

| Workload | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| rest, Uniform | 70-210 | 90-260 | 100-300 | 130-340 |
| rest, Clustered | 40 | 50 | 80 | 130-140 |

The `std::map` worst case is also much heavier-tailed: its per-pass maximum is
43-177 µs (allocator and tree-node cache misses) against 50-280 ns for FlatBook.
The `max` figure here is the median of the per-pass maxima, not the largest
value ever observed.

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
~18M orders/s with one worker (mixed crossing, 256 symbols) and near-linear
scaling with the worker count (~2.2× at 2, ~4.3× at 4, saturating near the core
count at 8). The single ingress and collector threads bound how far it scales.

## Graphs

Each chart plots latency vs the configured price range (`std::map` dashed,
FlatBook solid), for both uniform (adversarial wide-range) and clustered
(near-touch) patterns.

**1. Add (mixed crossing, 37-45% of orders fill).** `std::map`'s best case:
roughly 40% of the orders match and never insert, so its O(log n) insert is
amortized away.

![addOrder latency vs price range](add_vs_price_levels.png)

FlatBook is flat (~28 ns) across the full 256× range; `std::map` grows 73 → 110 ns.

**2. Resting-only (every order rests).** Pure allocation: every rest is a
tree-node `new` in `std::map`, one flat-array write in FlatBook.

![resting-only add vs price range](rest_vs_price_levels.png)

FlatBook ~7-14 ns; `std::map` grows 71 → 257 ns (8-18×).

**3. Add/cancel mix (25% cancel).**

![add/cancel mix vs price range](cancel_vs_price_levels.png)

FlatBook ~25 ns vs `std::map` ~73 ns at 1,024 levels.

**4. Per order type (FlatBook).** FOK on random flow mostly measures the cheap
reject path.

![order types](order_type.png)

**5. CppTrader optimized baseline (resting-only, log scale).** The CppTrader
line explodes on uniform - its sorted-vector levels pay an O(levels) scan plus
shift per far-away insert - while FlatBook stays flat.

![CppTrader vs price range](cpptrader_vs_price_levels.png)

One command to build, run and re-plot (requires `matplotlib`):

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
`docs/order_type.png`.

## Limitations of these numbers

- **Synthetic flow.** Prices are drawn from a uniform or clustered distribution,
  not a real day's LOBSTER/ITCH stream; the adversarial wide-range pattern is
  intentionally hard for flat arrays, the clustered one intentionally easy.
- **Single thread, single core.** The latency numbers are one thread on one
  core; they say nothing about multi-symbol throughput or contention.
- **Frequency and boost.** `./dev.sh tail` warns when the governor is not
  `performance` or Intel turbo is enabled; it does not detect AMD boost, so check
  it manually (see the boost setting in Test Environment).
- **Timer resolution.** The TSC on the test CPU ticks every 10 ns (47 cycles), so
  per-operation percentiles are multiples of 10 ns and values around 10 ns
  (rest) are upper bounds. Treat differences below ~10 ns in the tail tables as
  noise; use the Google Benchmark averages for the exact rest cost.
- **Baselines are containers, not full matchers.** `std::map` and CppTrader
  baselines maintain levels but do not match (no crossing, no order types, no
  STP); the comparison is on the resting-only path, which is where their data
  structures differ. See the next section for real matchers.
- **CI is noisy.** Regenerate on a quiet, dedicated machine before publishing.
- **Large L3 cache.** The test CPU has 96 MB of L3 (3D V-Cache), so most of the
  configurations here are largely cache-resident. On CPUs with a smaller L3 the
  wide-range Uniform cases (262,144 levels) will likely be slower; the Clustered
  cases touch a small working set and should be less affected.
- **Single machine.** All numbers come from one workstation; results on other
  CPUs and under virtualization will differ.

## Real matchers

Liquibook (a full callback-based matching engine, header-only, BSD-style
license) is benchmarked on the same resting-only `addLimit` stream, median of 3
runs:

| Price levels | FlatBook | std::map | Liquibook |
|---|---|---|---|
| 1,024 | 8.6 ns | 70.7 ns | 187 ns |
| 16,384 | 9.9 ns | 131 ns | 192 ns |
| 262,144 | 14.1 ns | 265 ns | 191 ns |

What is compared and how the semantics differ:

- **Same order stream**: Liquibook is driven with the same `addLimit(price, qty,
  buy)` workload as FlatBook and `std::map`; warm-up and reset are identical.
- **Liquibook is a real matcher**: it accepts an order object and reports fills
  through callbacks, so its timed path includes queueing a per-order object
  (its design requires a stable Order per order). FlatBook and `std::map` do not
  allocate on this path.
- **No resting order id**: Liquibook has no id to return from `add`, so the
  resting path is compared by depth (`bestBid`/`bestAsk`/`volume`), not ids;
  the add/cancel benchmark is therefore not included for Liquibook.
- **Semantics in common**: Limit + Market, price-time FIFO. Liquibook also has
  stop / all-or-none / IOC (not exercised); FlatBook also has post-only / FOK /
  STP / reason codes (not exercised). Both cross at the same prices.

Liquibook is ~190 ns and roughly flat across the price range (its internal
`std::multimap` of order trackers grows log-n), while FlatBook stays ~8-14 ns
via direct indexing. This is the honest matcher-to-matcher comparison; the
CppTrader / `std::map` rows above show only the container-level difference.