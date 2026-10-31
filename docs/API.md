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

## Semantics

Edge-case behavior is specified in [SEMANTICS.md](SEMANTICS.md); the safety model
(validated vs precondition-guarded) is in [DESIGN.md](DESIGN.md).
