# Public API

Header-only, `namespace flatbook`, C++20. Include one header; every header is
self-contained (verified by `./dev.sh headers`).

| Header | What it provides |
|---|---|
| `MatchingEngine.hpp` | `MatchingEngine<Book>`, `Result`, `Reason`, `AddResult`, `CancelResult`, `Fill`, order / TIF enums |
| `OrderBook.hpp` | `OrderBook<Size, PriceLevels, Id, Participant, Rebaseable>` (the data structure) |
| `MatchingDispatcher.hpp` | `MatchingDispatcher`, `DispatcherEvent`, `makeDispatcher`, `MatchingConfig`, `Stats` |
| `Lobster.hpp` | `LobsterReplay`, `LobsterFeed`, `parseLobsterLine`, `parseOrderbookLine`, `replayLobsterFile` |
| `LobsterValidator.hpp` | `validateLobster`, `matchesTopN` |
| `Id.hpp`, `IdPool.hpp` | `RawId`, `GenerationalId`, `IdPool` |
| `Bitmap.hpp`, `SpinWait.hpp`, `SpscQueue.hpp`, `Wait.hpp` | building blocks (also usable directly) |

## Internal vs public

- Everything in `flatbook::detail` (e.g. the futex primitives in `Wait.hpp`) is
  internal and not covered by the semver promise.

## Semver

`0.x` may break the API on a minor bump; `1.0.0` freezes it. See
[CHANGELOG.md](../CHANGELOG.md) for breaking changes.

## Semantics

Edge-case behavior is specified in [SEMANTICS.md](SEMANTICS.md); the safety model
(validated vs precondition-guarded) is in [DESIGN.md](DESIGN.md).
