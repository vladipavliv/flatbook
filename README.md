<h1 align="center">FlatBook - Deterministic Order Book</h1>

<p align="center">
  <a href="https://github.com/vladipavliv/flatbook/actions"><img src="https://img.shields.io/github/actions/workflow/status/vladipavliv/flatbook/ci.yml?branch=main&style=flat-square" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow?style=flat-square" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square" alt="Standard: C++20">
  <img src="https://img.shields.io/badge/header--only-ff69b4?style=flat-square" alt="Header-only">
  <a href="https://github.com/vladipavliv/flatbook/releases"><img src="https://img.shields.io/github/v/release/vladipavliv/flatbook?style=flat-square" alt="Release"></a>
</p>

**A header-only, deterministic order book and matching engine for C++20.**
Direct-indexed price levels, a hierarchical bitmap for best bid/ask and a
fixed-capacity order pool mean the hot path never allocates, locks or throws, so
latency stays flat as the book grows. Order ids can carry a generation, so a
stale id never cancels or matches the wrong order, and the same input always
produces the same fills and book state.

Around the core: self-trade prevention, post-only / IOC / FOK / AON / cancel-replace,
a sharded multi-symbol dispatcher, LOBSTER replay with a book validator, and
Python bindings with a numpy batch API. Tested on GCC, Clang and MSVC; CI also
runs ASan + UBSan, TSan, clang-tidy and libFuzzer.

| Operation (per call)                | p50      | p90      | p99      | p99.9     |
|:------------------------------------|---------:|---------:|---------:|----------:|
| **Add** (mixed flow, 37–45% fills)  | 20–30 ns | 30–50 ns | 50–90 ns | 80–130 ns |
| **Rest** (order lands in the book)  | 10–20 ns | 10–20 ns | 10–30 ns | 20–40 ns  |

<sub>32K–524K orders, 1K–262K price levels, uniform and clustered prices; AMD
Ryzen 7 9800X3D (96 MB L3), pinned core, 5 passes × 200,000 samples. The timer
ticks every 10 ns, so single-digit rest values are upper bounds. On a
resting-only stream FlatBook is 13–22× faster than
[Liquibook](https://github.com/objectcomputing/liquibook) and 8–19× faster than
a `std::map` book. Methodology and caveats: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).</sub>

## Contents

- [Quickstart](#quickstart)
- [Feature matrix](#feature-matrix)
- [Price window and rebase](#price-window-and-rebase)
- [Order ids and determinism](#order-ids-and-determinism)
- [Architecture](#architecture)
- [Testing](#testing)
- [Status and limitations](#status-and-limitations)
- [Performance](#performance)
- [Install / use as a dependency](#install--use-as-a-dependency)
- [Building](#building)
- [Documentation](#documentation)
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

Run the examples in [`examples/`](examples/) (`example_basic`, `example_dispatcher`, `example_lobster_replay`).

## Feature matrix

| Feature | Status |
|---|---|
| Limit / market orders, GTC / IOC / FOK / AON | ✅ |
| Post-only | ✅ |
| Self-trade prevention (participant keys, cancel-newest) | ✅ |
| Cancel / modify (cancel-replace) | ✅ |
| Price window + rebase | ✅ (optional, see below) |
| Deterministic sequence numbers | ✅ |
| Multi-symbol dispatcher (lock-free sharding) | ✅ |
| LOBSTER market-data replay + book validator | ✅ (benchmark / validation tool) |
| Python bindings (nanobind, numpy batch API) | ✅ |
| Fuzzing (libFuzzer: engine, parser) | ✅ |
| Stop / iceberg / hidden orders | ❌ |
| Pro-rata or multi-priority matching | ❌ (price-time only) |
| Amend that keeps time priority | ❌ (`modify` loses priority) |

Exchange-level concerns - trading sessions, opening/closing auctions, and
market-wide circuit breakers - are deliberately out of scope: this is a matching
engine, and those are the exchange's business logic to build on top of it (a
circuit-breaker *guard* is provided as an optional `CircuitBreaker` utility, not
a core feature).

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
fills and state - the foundation for replay and state comparison. See
[docs/DESIGN.md](docs/DESIGN.md) for the full design and the safety model.

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

## Testing

~170 unit and property tests plus libFuzzer targets, run under GCC, Clang and
MSVC; CI also runs ASan + UBSan, TSan, clang-tidy and `-Werror`. Line coverage
of `include/` is ~94% (gcovr, enforced at ≥85% in CI). Run them with
`./dev.sh test` / `sanitize` / `tsan` / `fuzz`.

## Status and limitations

Version **0.2.0** - the API is not yet stable; `0.x` releases may break it (see
Versioning). The core is single-threaded and deterministic: suitable for
backtesting, simulation and as a low-latency prototype. It is **not** a turnkey
exchange - no sessions/auctions/risk checks, one price-time priority, no
market-data distribution. Battle hardening (audit, real-feed replay validation,
external users) is the roadmap, not a TODO. Persistence is not part of this
release: the journal and disk checkpointing were removed and will be replaced by
an asynchronous write-ahead log.

## Performance

Benchmarked across different configurations and compared with `std::map`,
CppTrader and Liquibook on the same order streams (apples-to-apples). Full
methodology, per-workload tables and caveats:
[docs/BENCHMARKS.md](docs/BENCHMARKS.md).

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
FetchContent_Declare(flatbook GIT_REPOSITORY https://github.com/vladipavliv/flatbook.git GIT_TAG v0.2.0)
FetchContent_MakeAvailable(flatbook)
target_link_libraries(app PRIVATE flatbook::flatbook)
```

Local vcpkg and Conan manifests are included (`vcpkg.json`, `conanfile.py`); the
package is not yet published to the public registries.

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

## Documentation

- [API.md](docs/API.md) - the public API surface and the internal/public boundary.
- [SEMANTICS.md](docs/SEMANTICS.md) - edge-case behavior per order type / TIF, tied to tests.
- [DESIGN.md](docs/DESIGN.md) - data structures, order ids, determinism, safety model.
- [BENCHMARKS.md](docs/BENCHMARKS.md) - methodology, baselines, machine config.
- [VALIDATION.md](docs/VALIDATION.md) - LOBSTER data format and how to validate the book.

## Versioning

FlatBook follows [Semantic Versioning](https://semver.org): `0.x` may break the
API on minor bumps, `1.0.0` is the first stable release. See
[CHANGELOG.md](CHANGELOG.md).
