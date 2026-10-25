# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

While the project is in `0.x`, the minor version may introduce breaking API
changes (`0.1.0` → `0.2.0`); patch releases (`0.1.0` → `0.1.1`) are
backward-compatible fixes only.

## [Unreleased]

Initial development. No stable release yet.

### Added

- `OrderBook`: allocation-free, deterministic price-time priority book built on
  direct-indexed arrays and a hierarchical bitmap (O(1) best-bid/ask, O(1)
  insert/remove, flat latency).
- `MatchingEngine`: price-time crossing, order types (Limit/Market),
  time-in-force (GTC/IOC/FOK), post-only, self-trade prevention
  (cancel-newest), cancel/modify, monotonic sequence numbers and fill callbacks.
- `RawId` / `GenerationalId` (ABA protection) and `IdPool`.
- C++20 concepts `OrderIdLike` and `OrderBookLike`.
- `SpscQueue`: lock-free single-producer/single-consumer byte-stream ring with
  cached head/tail indices and batched read/write.
- `MatchingDispatcher`: sharded multi-symbol matching across worker threads
  (single ingress, single collector) with add/cancel/modify/rebase, correlation
  ids, non-blocking `try*` backpressure, runtime counters, a template (zero-cost)
  event callback, and a futex-based idle wait.
- Price window: logical `Price` (int64) mapped to a fixed-width slot window via
  a runtime `base_`; `rebase()` shifts it (optional, `Rebaseable=false` compiles
  it out); out-of-window limit prices are rejected with a reason.
- `AddResult::reason` (`Reason`: Invalid/PostOnly/Fok/SelfTrade/Capacity).
- `MatchingEngine::restOrder()` / `reduce()`: feed-driven "insert without
  matching" and execute-by-id primitives for market-data replay.
- `Journal` + `snapshot()`/`restore()`/`snapshotInto()`: deterministic operation
  journal and full-state snapshot, with `replayInto` round-trip.
- `Persist`: versioned, checksummed, atomic (temp+fsync+rename) snapshot + journal
  checkpoint to disk, with config-fingerprint validation.
- `Lobster` replay (`LobsterReplay` throughput benchmark and `LobsterFeed`
  feed-driven replay), orderbook parsing, and `LobsterValidator` for validating a
  feed against the LOBSTER orderbook file.
- `Wait`: portable futex wait/wake with a hybrid spin-then-block idle loop.
- Test suite: unit, container, engine, property, invariant-fuzz, journal,
  persistence, LOBSTER and dispatcher tests, run under ASan/UBSan, TSan and
  libFuzzer.
- CI matrix: GCC, Clang, Clang + libc++, MSVC, TSan, coverage, clang-tidy,
  libFuzzer, and a `-Werror` build.
- Benchmarks against a `std::map` baseline with latency-vs-price-range plots,
  plus per-op tail-latency percentiles and a dispatcher throughput/scaling bench.
