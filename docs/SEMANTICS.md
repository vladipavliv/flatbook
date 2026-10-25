# Semantics

This document specifies FlatBook's edge-case behavior. Each statement is tied to
a test in `tests/src/` (referenced as `TestFile.TestName`). If a statement has
no test, that is a bug in this document.

## Order types and time-in-force

| type × tif | crossing | remainder | Result |
|---|---|---|---|
| Limit × GTC | match at/better | rests | `Resting` / `Filled` |
| Limit × IOC | match | cancelled | `Filled` / `Cancelled` |
| Limit × FOK | all-or-nothing | — | `Filled` / `Rejected` |
| Market × GTC | match at best | cancelled (never rests) | `Filled` / `Cancelled` |
| Market × IOC | match | cancelled | `Filled` / `Cancelled` |
| Market × FOK | all-or-nothing | — | `Filled` / `Rejected` |

- Zero quantity → `Rejected`, reason `InvalidQuantity`
  (`TestOrderBook.RejectReasonCoverage`).
- `postOnly` is a resting-limit concept: it is only valid with Limit × GTC;
  anything else → `Rejected`, `InvalidPostOnly` (`TestOrderBook.RejectReasonCoverage`).
- `postOnly` that would cross → `Rejected`, `PostOnlyWouldCross`
  (`TestOrderBook.RejectReasonsAreDistinct`).
- Limit price outside the window → `Rejected`, `PriceOutOfWindow`
  (`TestOrderBook.PriceOutOfRangeRejected`).
- FOK that cannot fill fully → `Rejected`, `FokInsufficientLiquidity`
  (`TestOrderBook.RejectReasonsAreDistinct`). The FOK check is read-only: it sums
  the crossable levels' volume, excluding the aggressor's own orders under STP.

## Self-trade prevention (STP)

- `participant == 0` disables STP; a non-zero key enables it.
- **cancel-newest**: if the aggressor's remainder would cross its own resting
  orders (same non-zero participant), the remainder is cancelled, reason
  `StpCancelNewest`, instead of resting.
- STP only prunes fills against the aggressor's own orders; it never changes the
  price-time order of other participants' fills.

## Cancel and modify

- `cancelOrder` is sequenced and returns `CancelResult { cancelled, seq, reason }`;
  a stale id → `cancelled=false`, reason `UnknownOrder`
  (`TestOrderBook.RejectReasonCoverage`).
- `modify` is cancel-and-replace: the replacement must be Limit × GTC on the same
  side. It returns the replacement's `AddResult` with one of:
  - `UnknownOrder` — stale id,
  - `UnsupportedModify` — market / non-GTC replacement,
  - `SideMismatch` — side differs from the original
    (`TestOrderBook.RejectReasonCoverage`).
- `modify` inherits the original order's participant.
- `modify` **loses time priority**: the order is removed and re-added at the back
  of the (new) level.

## Sequence numbers

- `addOrder`, `cancelOrder`, `modify`, `rebase` each consume a sequence; the value
  is stamped on the result ack and every fill.
- `modify` consumes **two** sequences: one for the modify request, one for the
  replacement `addOrder` (`TestOrderBook.SequenceIsMonotonic`).
- `reduce` (feed-driven execute-by-id) does **not** consume a sequence.
- `clear()` resets the sequence counter to 1 (a new epoch); it does not reset the
  price window base.

## Price window and rebase

- Prices are logical `int64`; the window maps `slot = price - base_` and spans
  `[base_, base_ + PriceLevels)`.
- A limit price outside the window is rejected (see above), it never silently
  clamps or wraps.
- `rebase(newBase)` shifts the window; orders whose logical price falls outside
  the new window are cancelled and their ids returned. The caller is responsible
  for reporting those cancellations.
- `OrderBook<..., Rebaseable=false>` compiles `rebase()` out: calling it is a
  compile error, and the core is strictly allocation-free.

## Preconditions and NDEBUG

The API is split into validated and precondition-guarded parts:

- **Validated** — always safe: `insert`, `addOrder`, `modify`, `cancelOrder`
  reject bad input with a `Reason`.
- **Precondition-guarded** — assume a valid id / in-window price and are checked
  with `assert()` only: `qty`, `priceOf`, `sideOf`, `participantOf`, `order`,
  `level`, `head`, `volume`, `nextOf`, `nextLevel` (on a price), `IdPool::release`,
  and the engine's `reduce`. Under `NDEBUG` (Release) these become unchecked and
  violating a precondition is undefined behavior.

Build with the sanitizer profile (`./dev.sh sanitize`) to keep `assert` enabled
and add ASan/UBSan during development.
