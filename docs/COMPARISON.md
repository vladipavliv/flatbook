# Comparison

FlatBook is compared against `std::map` and CppTrader reference books and
Liquibook on the same order streams and book sizes - apples-to-apples: same
prices, sides and crossing rate. Same test environment and methodology as
[BENCHMARKS.md](BENCHMARKS.md).

## Feature comparison

`std::map` and CppTrader are containers, not matchers; they are included only as
data-structure baselines on the resting path.

| Capability | FlatBook | Liquibook | CppTrader | std::map |
|---|---|---|---|---|
| Matching engine (crossing, fills) | ✅ | ✅ | — | — |
| Limit / market orders | ✅ | ✅ | — | — |
| IOC / FOK / AON | ✅ | IOC / AON | — | — |
| Post-only | ✅ | — | — | — |
| Self-trade prevention | ✅ | — | — | — |
| Deterministic sequence numbers | ✅ | — | — | — |
| Generational order ids | ✅ | — | — | — |
| Multi-symbol dispatcher | ✅ | — | — | — |
| Allocation-free hot path | ✅ | — | — | — |
| Python bindings | ✅ | — | — | — |

`—` = not provided / not applicable. Only features exercised by this project's
benchmarks are listed for Liquibook; it also has stop orders (not measured here).

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

`std::map` rest tail latency, at 262,144 orders (same methodology as
[BENCHMARKS.md](BENCHMARKS.md)):

| Workload | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| rest, Uniform | 70-210 | 90-260 | 100-300 | 130-340 |
| rest, Clustered | 40 | 50 | 80 | 130-140 |

The `std::map` worst case is also much heavier-tailed: its per-pass maximum is
43-177 µs (allocator and tree-node cache misses) against 50-280 ns for FlatBook.
The `max` figure here is the median of the per-pass maxima, not the largest
value ever observed.

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

## Real matchers (Liquibook)

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

## Comparison graphs

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

**4. CppTrader optimized baseline (resting-only, log scale).** The CppTrader
line explodes on uniform - its sorted-vector levels pay an O(levels) scan plus
shift per far-away insert - while FlatBook stays flat.

![CppTrader vs price range](cpptrader_vs_price_levels.png)
