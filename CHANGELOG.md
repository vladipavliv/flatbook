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
  (single ingress, single collector) with add/cancel/modify.
- Test suite: unit, container, engine, property and invariant-fuzz tests
  (110 tests), run under ASan/UBSan and TSan.
- CI matrix: GCC, Clang, Clang + libc++, MSVC, TSan, and coverage.
- Benchmarks against a `std::map` baseline with latency-vs-price-range plots,
  plus per-op tail-latency percentiles and a dispatcher throughput/scaling bench.
