<h1 align="center">FlatBook - Deterministic Order Book</h1>

<p align="center">
  <a href="https://github.com/vladipavliv/flatbook/actions"><img src="https://img.shields.io/github/actions/workflow/status/vladipavliv/flatbook/ci.yml?branch=main&style=flat-square" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow?style=flat-square" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square" alt="Standard: C++20">
  <img src="https://img.shields.io/badge/header--only-ff69b4?style=flat-square" alt="Header-only">
  <a href="https://github.com/vladipavliv/flatbook/releases"><img src="https://img.shields.io/github/v/release/vladipavliv/flatbook?style=flat-square" alt="Release"></a>
</p>

A header-only, allocation-free order book and matching engine for C++20, built
around direct-indexed arrays and a hierarchical bitmap.

On top of the core, FlatBook ships a sharded multi-symbol dispatcher, a
deterministic journal with snapshot/restore and disk persistence for replay and
recovery, LOBSTER market-data replay with a book validator, and Python bindings
with a numpy batch API. Benchmarked against `std::map`, CppTrader and Liquibook.

Tested with GCC / Clang (libstdc++ & libc++) / MSVC; CI runs ASan+UBSan, TSan, clang-tidy, libFuzzer and coverage.

## Contents

- [Quickstart](#quickstart)
- [Feature matrix](#feature-matrix)
- [Price window and rebase](#price-window-and-rebase)
- [Order ids and determinism](#order-ids-and-determinism)
- [Architecture](#architecture)
- [Status and limitations](#status-and-limitations)
- [Performance](#performance)
- [Install / use as a dependency](#install--use-as-a-dependency)
- [Building](#building)
- [Versioning](#versioning)

## Quickstart

```cpp
#include <flatbook/MatchingEngine.hpp>

using Book   = flatbook::OrderBook<1 << 15, 1 << 10>; // 32k orders, 1024 price levels
using Engine = flatbook::MatchingEngine<Book>;

Engine engine;

engine.addOrder({
    .type = Engine::OrderType::Limit,
    .tif  = Engine::TimeInForce::GTC,
    .price = 100, .quantity = 10, .side = Engine::Side::Buy,
}, [](const Engine::Fill &f) { /* f.orderId, f.price, f.quantity, f.side */ });

auto r = engine.addOrder({/* ... */});
switch (r.result) {
case Engine::Result::Filled:    break; // fully matched
case Engine::Result::Resting:   break; // r.orderId is the resting id
case Engine::Result::Cancelled: break; // IOC / market remainder discarded
case Engine::Result::Rejected:  break; // nothing executed; see r.reason
}
```

> Large books store their arrays inline (hundreds of KB) - allocate them on the
> heap (or `static`), not the stack.

Run the examples in [`examples/`](examples/) (`example_basic`, `example_dispatcher`, `example_persistence`, `example_lobster_replay`).

## Feature matrix

| Feature | Status |
|---|---|
| Limit / market orders, GTC / IOC / FOK | ✅ |
| Post-only | ✅ |
| Self-trade prevention (participant keys, cancel-newest) | ✅ |
| Cancel / modify (cancel-replace) | ✅ |
| Price window + rebase | ✅ (optional, see below) |
| Deterministic sequence numbers + journal + snapshot/restore | ✅ |
| Disk persistence (checkpoint + journal tail) | ✅ |
| Multi-symbol dispatcher (lock-free sharding) | ✅ |
| LOBSTER market-data replay + book validator | ✅ (benchmark / validation tool) |
| Python bindings (nanobind, numpy batch API) | ✅ |
| Fuzzing (libFuzzer: engine, persistence, parser) | ✅ |
| Sessions, auctions, circuit breakers | ❌ |
| Stop / iceberg / hidden orders | ❌ |
| Pro-rata or multi-priority matching | ❌ (price-time only) |
| Amend that keeps time priority | ❌ (`modify` loses priority) |

## Price window and rebase

Prices are logical `int64` ticks. The book maps them into a fixed-width window
via `slot = price - base_`: slot 0 is `base_`, and the window spans
`[base_, base_ + PriceLevels)`.

- A limit price outside the window is **rejected** (`Result::Rejected`, `Reason::PriceOutOfWindow`).
- `rebase(newBase)` shifts the window so slot 0 == `newBase`; orders that fall
  outside the new window are cancelled and their ids returned.

For a venue with a fixed price band, size `PriceLevels` to cover the band and
reject everything else - no rebase needed. For a simulator with unbounded
prices, `rebase()` follows the price (a cold O(n) operation). Instantiate with
`OrderBook<..., Rebaseable=false>` to compile `rebase()` out entirely and keep
the core strictly allocation-free.

## Order ids and determinism

`addOrder` returns an order id. The default `RawId<Size>` is a plain dense index
(no overhead) - enough when an id is used only within one synchronous call. The
moment an id crosses an async boundary (a later `cancel` referencing an earlier
order), two hazards appear: the order may have been **closed** (id freed), or
its slot **recycled** for a different order. A stale id must then be a no-op,
not a wrong match.

`GenerationalId<Size, GenBits>` packs a per-slot generation into the id and
bumps it on every reuse, so a stale id never aliases the slot's current
occupant. Use it whenever ids escape a synchronous call - including the
`MatchingDispatcher`, which requires it (`static_assert`).

```cpp
using GenBook = OrderBook<1 << 15, 1 << 10, GenerationalId<1 << 15, 24>>;
```

The matching engine is fully **deterministic**: a monotonic `Sequence` is stamped
on every result and fill, ids are recycled LIFO, and there is no hashing,
randomness or clock. Replaying the same operations reproduces the same ids,
fills and state - the foundation for the journal, snapshot/restore and
persistence. See [docs/DESIGN.md](docs/DESIGN.md) for the full design and the
safety model.

## Architecture

The core hot path is allocation-free: no heap allocation, no exceptions, no
locks per order, with O(1) best-bid/best-ask and insert/remove. The one cold
exception is `rebase()` (an explicit window-shift), which allocates a vector of
cancelled ids and can be compiled out with `OrderBook<..., Rebaseable=false>`.

Three layers:

- **`OrderBook`** - a pure price-time priority data structure: ordered levels,
  FIFO within a level, O(1) best-bid/best-ask and O(1) insert/remove. No
  crossing, no order-type semantics, no events.
- **`MatchingEngine<Book>`** - the matching layer: price-time crossing, order
  types, time-in-force, self-trade prevention and fill reporting.
- **`MatchingDispatcher<SymbolCount, Book>`** - the sharding layer: `SymbolCount`
  engines across worker threads, fanned back into one callback thread.

Fills are reported through a caller-supplied callback (a template parameter, no
`std::function`), so the engine stays allocation-free on the hot path.

## Status and limitations

Version **0.2.0** - the API is not yet stable; `0.x` releases may break it (see
Versioning). The core is single-threaded and deterministic: suitable for
backtesting, simulation and as a low-latency prototype. It is **not** a turnkey
exchange - no sessions/auctions/risk checks, one price-time priority, no
market-data distribution. Battle hardening (audit, real-feed replay validation,
external users) is the roadmap, not a TODO.

## Performance

Google Benchmark, `-O3` (add `-DFLATBOOK_NATIVE=ON` for `-march=native`), `CMAKE_BUILD_TYPE=Release`.
Add-latency (median) at a glance:

| BookSize / PriceLevels | Uniform | Clustered |
|---|---|---|
| 32,768 / 1,024 | 27.0 ns | 20.2 ns |
| 131,072 / 16,384 | 27.2 ns | 18.9 ns |
| 524,288 / 262,144 | 27.7 ns | 19.2 ns |

Full methodology, per-workload tables, `std::map` / CppTrader baselines, tail
latency and graphs: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Install / use as a dependency

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(flatbook REQUIRED)
target_link_libraries(app PRIVATE flatbook::flatbook)
```

Or vendor with FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(flatbook GIT_REPOSITORY https://github.com/vladipavliv/flatbook.git)
FetchContent_MakeAvailable(flatbook)
target_link_libraries(app PRIVATE flatbook::flatbook)
```

Also packaged for vcpkg (`vcpkg.json`) and Conan (`conanfile.py`).

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFLATBOOK_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build            # unit tests
./build/benchmarks/flatbook_bench  # benchmarks
```

Handy one-liners (see `dev.sh`):

```bash
./dev.sh test        # Release build + unit tests
./dev.sh sanitize    # Debug + ASan/UBSan + tests
./dev.sh tsan        # Debug + TSan + tests
./dev.sh fuzz [sec]  # libFuzzer build + short fuzz run
./dev.sh tidy        # clang-tidy over the headers
./dev.sh bench       # benchmarks + graphs (docs/)
./dev.sh tail        # per-op latency percentiles (p50..p99.9/max)
./dev.sh all         # test + sanitize + bench
```

## Versioning

FlatBook follows [Semantic Versioning](https://semver.org): `0.x` may break the
API on minor bumps, `1.0.0` is the first stable release. See
[CHANGELOG.md](CHANGELOG.md).
