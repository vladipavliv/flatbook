# Public API

Header-only, `namespace flatbook`, C++20. Include one header; every header is
self-contained (verified by `./dev.sh headers`).

| Header | What it provides |
|---|---|
| `Types.hpp` | `Price`, `Quantity`, `Volume`, `Sequence` (fixed-width value types) |
| `Concepts.hpp` | `OrderIdLike`, `OrderBookLike` (compile-time contracts) |
| `MatchingEngine.hpp` | `MatchingEngine<Book>`, `Result`, `Reason`, `AddResult`, `CancelResult`, `Fill`, order / TIF enums |
| `OrderBook.hpp` | `OrderBook<Size, PriceLevels, Id, Participant, Rebaseable>` (the data structure) |
| `MatchingDispatcher.hpp` | `MatchingDispatcher`, `DispatcherEvent`, `makeDispatcher`, `MatchingConfig`, `Stats` |
| `CircuitBreaker.hpp` | `CircuitBreaker<Engine>` (price band + volatility halt wrapper) |
| `Lobster.hpp` | `LobsterReplay`, `LobsterFeed`, `parseLobsterLine`, `parseOrderbookLine`, `replayLobsterFile` |
| `LobsterValidator.hpp` | `validateLobster`, `matchesTopN` |
| `Id.hpp`, `IdPool.hpp` | `RawId`, `GenerationalId`, `IdPool` |
| `Bitmap.hpp`, `SpinWait.hpp`, `SpscQueue.hpp`, `Parker.hpp`, `Threading.hpp` | building blocks (also usable directly) |

## Internal vs public

- Only the symbols listed in the table above are public and covered by the
  semver promise; private members and implementation details are not.

## Semver

`0.x` may break the API on a minor bump; `1.0.0` freezes it. See
[CHANGELOG.md](../CHANGELOG.md) for breaking changes.

## API stability (the 1.0 freeze contract)

- **Enums** (`Result`, `Reason`, `OrderType`, `TimeInForce`): values may be
  **added** in a minor release, but never renamed or reordered. Treat them as
  an open set (add a `default:` to exhaustive switches).
- **`DispatcherEvent`** and its `CancelEvent` / `RebaseEvent` payloads: the
  layout is part of the SPSC contract, so it is stable and trivially copyable.
- **`Snapshot`**: an in-process state capture, **not** a stable serialization
  format. Its layout may change without a semver bump; it is for save/restore
  and state comparison, not a file format.
- **Release vs UB**: precondition-guarded accessors (`qty`, `priceOf`, `sideOf`,
  `participantOf`, `reduce`, ...) assert in debug builds and have undefined
  behavior on invalid input in `Release`. Call them only on live ids / in-window
  prices; the safety model is documented in [DESIGN.md](DESIGN.md).

## Semantics

Edge-case behavior is specified in [SEMANTICS.md](SEMANTICS.md); the safety model
(validated vs precondition-guarded) is in [DESIGN.md](DESIGN.md).
