# FlatBook - Deterministic Order Book

A header-only, allocation-free order book for C++23, built around direct-indexed arrays and a hierarchical bitmap.
No heap allocation, no exceptions, no locks in the core - with flat per-operation latency (best-bid/ask and insert/remove are O(1)) - plus an optional lock-free sharding layer.

[![CI](https://github.com/vladipavliv/flatbook/actions/workflows/ci.yml/badge.svg)](https://github.com/vladipavliv/flatbook/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Standard: C++23](https://img.shields.io/badge/Standard-C%2B%2B23-blue.svg)]()
[![Header-only](https://img.shields.io/badge/header--only-ff69b4.svg)]()

> **Core is single-threaded by design.** `OrderBook` and `MatchingEngine` have
> no locks, no atomics and no internal synchronization - pin one to a thread and
> feed it a serialized order stream. `MatchingDispatcher` (see below) shards
> `SymbolCount` engines across worker threads with lock-free SPSC queues.

## Why

The goal is a flat latency profile: every operation - insert, remove, cancel, best-bid/best-ask - is O(1) regardless of access pattern or price distribution. A wide, adversarial range (orders scattered far from the touch) costs the same as a tight, clustered one.

Tree- and hash-based books cannot guarantee that. `std::map` levels are O(log n) with a pointer chase per node, `std::unordered_map` cannot even yield the best price without an O(levels) scan or a second sorted structure, and both scatter memory across the heap - so latency becomes a function of allocator behavior as much as of the algorithm itself.

FlatBook removes the variable: every price level and every order slot has a fixed, known-in-advance position in a flat array, and best-bid/best-ask is a hierarchical bitmap lookup. No resizing, no rehashing, no allocation on the hot path. The price range and order capacity are compile-time template parameters - you pay for exactly the memory you configure, and the cost stays constant everywhere.

## Architecture

The library is split into three layers:

- **`OrderBook`** - a pure price-time priority data structure. It maintains ordered levels, FIFO within a level, O(1) best-bid/best-ask and O(1) insert/remove. No crossing, no order-type semantics, no events.
- **`MatchingEngine<Book>`** - the matching layer. It owns a book and implements price-time crossing, order types, time-in-force and fill reporting on top of the book's primitives.
- **`MatchingDispatcher<SymbolCount, Book>`** - the sharding layer. It owns `SymbolCount` engines, shards them across worker threads, and fans every event back into one callback thread.

Fills are reported through a caller-supplied callback, so the engine stays allocation-free on the hot path.

```cpp
#include <flatbook/MatchingEngine.hpp>

using Book = flatbook::OrderBook<1 << 15, 1 << 10>;
using Engine = flatbook::MatchingEngine<Book>;

Engine engine;

(void)engine.addOrder({
    .type     = Engine::OrderType::Limit,
    .tif      = Engine::TimeInForce::GTC,
    .postOnly = false,
    .price    = 100,
    .quantity = 10,
    .side     = Engine::Side::Buy,
}, [](const Engine::Fill &fill) {
    // fill.orderId, fill.price, fill.quantity, fill.side
});

auto result = engine.addOrder({/* ... */});
switch (result.result) {
case Engine::Result::Filled:    /* fully matched */
case Engine::Result::Resting:   /* remainder rested; result.orderId */
case Engine::Result::Cancelled: /* IOC-style: remainder discarded */
case Engine::Result::Rejected:  /* nothing executed */
}
```

### Order types

An order is described by an `OrderType`, a `TimeInForce` and an optional `postOnly` flag.

**OrderType**

| Type | Description |
|---|---|
| `Limit` | Price-resting order. Matches against the opposite side at or better than `price`, then rests any remainder. |
| `Market` | Marketable order. Matches against the opposite side at whatever price is available; never rests. |

**TimeInForce**

| TIF | Description |
|---|---|
| `GTC` | Good-til-cancelled. Match now, rest the unfilled remainder (limit only). |
| `IOC` | Immediate-or-cancel. Match as much as possible now, discard the rest. |
| `FOK` | Fill-or-kill. Fill the entire quantity immediately, or reject without executing anything. |

**postOnly** (Limit + GTC only)

Rests the order without ever crossing the opposite side. If it would match immediately, the order is rejected instead. Any other combination with `postOnly` is rejected.

**Result of `addOrder`**

| Result | Meaning |
|---|---|
| `Filled` | Fully matched, nothing left. |
| `Resting` | Remainder resting in the book (`orderId` holds the resting id). |
| `Cancelled` | Unfilled remainder discarded (IOC, or market with leftover). |
| `Rejected` | Nothing executed (invalid input, FOK impossible, post-only would cross). |

## Multi-threaded dispatcher

For many symbols, `MatchingDispatcher<SymbolCount, Book>` shards `SymbolCount`
engines across a configurable number of worker threads. Orders for a symbol are
always processed by the worker that owns that symbol (per-symbol order is
preserved), and all fills/results/cancel acks are fanned back into a single
collector thread, so the callback is never invoked concurrently:

```cpp
#include <flatbook/MatchingDispatcher.hpp>

using Book = flatbook::OrderBook<1 << 15, 1 << 10>;
using Dispatcher = flatbook::MatchingDispatcher<1024, Book>; // 1024 symbols

Dispatcher dispatcher({.workerCores = {0, 1, 2, 3}},
    [](const Dispatcher::Event &ev) {
        // ev.kind  : Fill | Result | Cancel
        // ev.symbolIdx, ev.fill / ev.result / ev.cancelled
    });

dispatcher.start();
dispatcher.addOrder(symbolIdx, order);        // async
dispatcher.cancelOrder(symbolIdx, orderId);   // async
dispatcher.modify(symbolIdx, orderId, repl);  // async
dispatcher.stop();
```

Submit orders from a single ingress thread; events arrive on one collector
thread in per-symbol order. `workerCores` pins workers to cores and `realTime`
switches them to `SCHED_FIFO` (both Linux, best-effort).

**No dynamic rerouting.** The symbol → worker assignment is fixed (round-robin).
Dynamic load rebalancing would either pause a symbol mid-move or add per-order
overhead - both break the flat-latency guarantee - so it is deliberately left to
the deployment layer (static partitioning, or multiple dispatcher instances
behind a router).

The lock-free queue backing the dispatcher is `SpscQueue`
(`include/flatbook/SpscQueue.hpp`): a bounded byte-stream SPSC ring with cached
head/tail indices and batched read/write.

**Throughput.** End-to-end the dispatcher moves ~18M orders/s with one worker
(mixed-crossing, 256 symbols; `flatbook_dispatcher_bench`) and scales
near-linearly with the worker count: ~2.2× at 2 workers, ~4.3× at 4, saturating
around the core count at 8. The single ingress and collector threads bound how
far it scales, but the matching workers are the part that parallelizes.

## Order ids: ABA-safe handles with generation

The book hands out an **order id** from `addOrder`. In a single-threaded, synchronous flow that id is only ever used within one call, so a plain dense index is enough - that is the default.

The moment an id escapes a synchronous call (a gateway sends an order to the engine, then later sends a `cancel` referencing that id over an async boundary), two stale-id hazards appear:

1. the order was **closed** and its slot freed - a late `cancel` must be a no-op, not a match;
2. the slot was **recycled** for a *different* order - a late `cancel` must not hit the wrong order.

The fix is a **generation**: pack a per-slot counter into the id and bump it on every reuse, so a stale id (old generation) never aliases the slot's current occupant.

```cpp
using GenBook = flatbook::OrderBook<1 << 15, 1 << 10,
                                    flatbook::GenerationalId<1 << 15, 24>>;
```

### How it works

- **The generation lives in the id; the pool just bumps it.** The free list stores full ids, generation included. `release(id)` pushes the id back verbatim; `acquire()` pops it and calls `bumpGeneration()`. The id type owns the counter and how it wraps; the pool only advances it on reuse. Nothing else in the library touches the counter.
- **The book only stores and compares.** A slot stores its generation only when the id has one; `remove(id)` rejects when `node.gen != id.generation()`.
- **One comparison covers all three cases.** `closed` (generation already moved on), `recycled` (moved on again), and `live` (matches) are all distinguished by the same `==` - no separate "alive/dead" status enum.

### Zero-cost by default

`RawId<Size>` (the default) is a thin wrapper whose `generation()` is a constant `0` and `bumpGeneration()` is a no-op. The generation field in each slot is a `[[no_unique_address]]` empty type, so the non-generational book is byte-for-byte identical to a plain index book - liveness is still just `qty != 0`.

| Id type | `Node` (32K/1K) | `Node` (131K/16K) | order id |
|---|---|---|---|
| `RawId` (default) | 12 B | 16 B | 2 / 4 B |
| `GenerationalId` (default 32 gen bits) | 16 B | 20 B | 8 B |

You pay +4 bytes per slot (the generation counter) only when you actually ask for ABA protection.

### Why plain LIFO + a wide generation

LIFO recycling is deliberate: it reuses the most recently freed slots, keeping the hot working set in cache. Its only downside is that those same hot slots get their generation bumped fastest, so a narrow counter would wrap sooner. Rather than slow that down - a LIFO→FIFO hybrid adds a branch on the hot path to work around a problem you can instead avoid - the counter is simply made wide: `GenerationalId` defaults to **32 generation bits ≈ 4.3B reuses per slot** before wrapping, and the width is a template parameter you can raise.

## Design

- **Direct-indexed price levels.** `levels_[price]` - O(1), no hashing, no collisions, no load-factor degradation. The trade-off is explicit: you reserve memory for the entire configured tick range up front.
- **Hierarchical (multi-level) bitmap** for best-bid/best-ask discovery. Each level is a 64:1 summary of the level below, down to a single word, so `min()`/`max()`/`findFirst()`/`findLast()` walk a constant number of levels regardless of how wide the occupied range is. The same class of trick used in the Linux O(1) scheduler's priority bitmaps and in buddy-allocator free-lists.
- **Free-list order-slot recycling.** Order IDs are indices into a flat node array; released IDs go onto a stack and are reused LIFO, keeping the working set of recently-touched nodes hot in cache.
- **Byte-conscious layout.** Index types (`OrderIdx`, `PriceIdx`) resolve to `uint16_t` or `uint32_t` at compile time depending on the configured capacity (`IndexType<Size>`).

## Safety model

FlatBook splits its API into two categories by how they handle bad input:

- **Validated** - always safe, no preconditions. `OrderBook::insert()` and
  `MatchingEngine::addOrder()` / `modify()` reject invalid input (zero quantity,
  out-of-range price, bad order-type/TIF combinations) and return
  `INVALID_ORDER` / `Result::Rejected` instead of misbehaving.
- **Precondition-guarded** - cheap accessors and trusted internals that assume a
  valid id / in-range index: `qty()`, `priceOf()`, `sideOf()`, `participantOf()`,
  `order()`, `level()`, `nextOf()`, `IdPool::release()`,
  `Bitmap::set/reset/contains` and the engine's private `reduce()`. These are
  checked with `assert()` and compile out under `NDEBUG`.

In a Release build (`NDEBUG`), violating a precondition is undefined behavior
(typically an out-of-bounds read/write). The preconditions hold by construction
when you follow the intended flow: keep the id returned by
`insert()` / `addOrder()` and check `isLive()` before cancel/modify. To catch
violations in development, build with the sanitizer profile (`./dev.sh
sanitize`), which keeps `assert` enabled and adds ASan/UBSan.

## Performance

Measured with Google Benchmark, GCC 15, `-O3 -march=native`, `CMAKE_BUILD_TYPE=Release`. The benchmark sweeps three book configurations and two price-generation modes (see `benchmarks/`):

- **Uniform** - prices drawn uniformly across the full configured tick range. An adversarial stress pattern that exposes how the structure behaves when the occupied range is wide.
- **Clustered** - a synthetic mid-price random-walks slowly, each order's price a small gaussian offset from it, approximating real microstructure.

| BookSize / PriceLevels | Uniform | Clustered |
|---|---|---|
| 32,768 / 1,024 | 27.0 ns | 20.2 ns |
| 131,072 / 16,384 | 27.2 ns | 18.9 ns |
| 524,288 / 262,144 | 27.7 ns | 19.2 ns |

Numbers are single-run medians and illustrative rather than a rigorous statistical claim; reproduce with `--benchmark_repetitions=10 --benchmark_report_aggregates_only=true` on your own hardware.

**Why it scales.** Best-bid/best-ask discovery is O(1) via the multi-level bitmap, and insert/remove are O(1), so latency is flat across the full 256x price range (27.0 → 27.7 ns uniform). The configured `levels_` array grows with `PriceLevels`, but the working set touched per operation stays small.

A `std::map`-based baseline (`BM_MapLatency`, same workload, with an `unordered_map` id→level index so its cancel is not a whole-book scan) shows the contrast on the uniform pattern:

| Price levels | FlatBook | std::map |
|---|---|---|
| 1,024 | 27.0 ns | 73.3 ns |
| 16,384 | 27.2 ns | 91.2 ns |
| 262,144 | 27.7 ns | 109.2 ns |

The map grows with the price range (every level lookup chases a tree node, every insert allocates); FlatBook stays flat because its price discovery is a constant number of bitmap levels and it allocates nothing.

The add-only, 50/50 crossing mix above is the *best case* for `std::map` - half the orders match and never insert. The per-workload picture (uniform, 1,024 levels):

| Workload | FlatBook | std::map | ratio |
|---|---|---|---|
| add, mixed crossing (`BM_Latency`/`BM_MapLatency`) | 27.0 ns | 73.3 ns | 2.7× |
| add, resting-only (`BM_RestLatency`/`BM_MapRestLatency`) | 7.7 ns | 72.4 ns | 9.4× |
| add/cancel mix, 25% cancel (`BM_Cancel`/`BM_MapCancel`) | 25.7 ns | 73.0 ns | 2.8× |

The resting gap widens with `PriceLevels` (7.7 → 12.3 ns FlatBook vs 72 → 278 ns map at 262,144 levels): every rest in a map is a tree-node `new`, FlatBook writes one flat-array slot. The map's cancel is an O(1) index lookup plus an O(k) scan of the level queue plus an erase; FlatBook cancels by id in O(1).

As a second baseline, FlatBook is also measured against
[CppTrader](https://github.com/chronoxor/CppTrader)'s optimized market manager -
the same flat-array + sorted-price-level technique, vendored under its MIT
license in `benchmarks/src/reference/cpptrader/`. It is a market-data replay
container (it maintains levels but does not match), so the comparison is on the
resting-only path. Median of 10 runs:

| Price levels | FlatBook | std::map | CppTrader |
|---|---|---|---|
| 1,024 | 8.5 ns | 69.9 ns | 283.4 ns |
| 16,384 | 9.6 ns | 131.1 ns | 4,244 ns |
| 262,144 | 13.4 ns | 270.5 ns | 11,359 ns |

Why it blows up: CppTrader stores price levels in a `std::vector` sorted by
price and finds a level by scanning linearly back from the best price, and
inserting a *new* level shifts every element after it (a memmove). A far-away
level therefore pays two O(levels) penalties - a scan plus a shift - where
FlatBook pays neither: price *is* the array index (O(1) insert), and the
hierarchical bitmap finds the best price in O(1). Near the touch (clustered)
the scan is short and the insert lands at the end (~105 ns, flat); on
wide-range uniform flow both penalties bite, and it degrades from 283 ns to
11.4 µs.

### Tail latency

Means and medians hide the worst case, so `./dev.sh tail` measures per-operation
latency percentiles (TSC-based, 100k samples per row, FlatBook `BookSize`
262,144):

| Workload | p50 | p99.9 |
|---|---|---|
| FlatBook add (mixed crossing) | 10-20 ns | 70-130 ns |
| FlatBook rest | ~10 ns | 20-40 ns |
| std::map rest | 40-210 ns | 120 ns - 2 µs |
| CppTrader rest (uniform) | 0.4-13 µs | 0.9-35 µs |

FlatBook's tail stays in tens-to-hundreds of nanoseconds; the reference
containers blow into microseconds on wide price ranges. The `max` column (not
shown) is a rare 1-in-100k scheduler/cache outlier, not the book itself - clean
tails need core pinning and fixed CPU frequency, so treat these as indicative,
like the averages above.

### Running the benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/benchmarks/flatbook_bench                       # everything
./build/benchmarks/flatbook_bench --benchmark_filter=BM_Latency
./build/benchmarks/flatbook_bench --benchmark_filter=BM_OrderType
```

The order-type sweep measures `addOrder` per type (`LimitGTC`, `Market`, `LimitIOC`, `LimitFOK`, `PostOnly`). Note: on random flow, FOK orders are mostly rejected immediately, so its number reflects the cheap reject path, not a deep-book full fill.

### Graphs

Each chart plots latency vs the configured price range (`std::map` in dashed lines, FlatBook in solid), for both the uniform (adversarial wide-range) and clustered (near-touch) patterns.

**1. Add (mixed 50/50 crossing).** This is `std::map`'s best case: half the orders match and never insert, so its O(log n) insert is amortized away.

![addOrder latency vs price range](docs/add_vs_price_levels.png)

FlatBook is flat (~27 ns) across the full 256× range; `std::map` grows 73 → 109 ns because each rest still chases the tree and maintains its id index.

**2. Resting-only (every order rests).** Exercises pure allocation: every rest is a tree-node `new` in `std::map`, one flat-array write in FlatBook.

![resting-only add vs price range](docs/rest_vs_price_levels.png)

FlatBook is ~8-13 ns; `std::map` grows 72 → 278 ns (9-22×), the tree deepening with distinct price levels.

**3. Add/cancel mix (25% cancel).** The map's cancel is an O(1) index lookup plus an O(k) scan of the level queue plus an erase; FlatBook cancels by id in O(1).

![add/cancel mix vs price range](docs/cancel_vs_price_levels.png)

FlatBook ~26 ns vs `std::map` ~73 ns at 1,024 levels - a ~2.8× gap on the mix (the map's cancel is no longer a whole-book scan, see `BM_MapCancel`).

**4. Per order type (FlatBook).** `addOrder` latency by type; FOK on random flow mostly measures the cheap reject path.

![order types](docs/order_type.png)

**5. CppTrader optimized baseline (resting-only, log scale).** The CppTrader line explodes on uniform - its sorted-vector levels pay an O(levels) scan plus shift per far-away insert - while FlatBook stays flat.

![CppTrader vs price range](docs/cpptrader_vs_price_levels.png)

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

Writes `docs/add_vs_price_levels.png`, `docs/rest_vs_price_levels.png`, `docs/cancel_vs_price_levels.png`, `docs/cpptrader_vs_price_levels.png` and `docs/order_type.png`. Regenerate on a quiet machine before release, since CI runners are noisy.

## Install / use as a dependency

Install to a prefix and consume with `find_package`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(flatbook REQUIRED)
target_link_libraries(app PRIVATE flatbook::flatbook)
```

Or vendor directly with FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(flatbook GIT_REPOSITORY https://github.com/vladipavliv/flatbook.git)
FetchContent_MakeAvailable(flatbook)
target_link_libraries(app PRIVATE flatbook::flatbook)
```

## Versioning

FlatBook follows [Semantic Versioning](https://semver.org). While in `0.x`, the
minor version may introduce breaking API changes (`0.1.0` → `0.2.0`); patch
releases (`0.1.0` → `0.1.1`) are backward-compatible fixes. The first stable
release will be `1.0.0`, after which breaking changes require a major bump.
The version is defined once in `CMakeLists.txt` and drives the generated
`find_package` version file. See [CHANGELOG.md](CHANGELOG.md).

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLATBOOK_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build            # unit tests
./build/benchmarks/flatbook_bench  # benchmarks
```

Handy one-liners (see `dev.sh`):

```bash
./dev.sh test               # Release build + unit tests
./dev.sh sanitize           # Debug + ASan/UBSan + tests
./dev.sh bench [--no-build] # FlatBook benchmarks + docs/*.png
./dev.sh bench --refs       # + std::map & CppTrader baselines (slower)
./dev.sh tail               # per-op latency percentiles (p50..p99.9/max)
./dev.sh all                # everything above
```
