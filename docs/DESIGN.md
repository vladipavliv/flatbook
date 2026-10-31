# Design

## Order ids: ABA-safe handles with generation

The book hands out an **order id** from `addOrder`. In a single-threaded,
synchronous flow that id is only ever used within one call, so a plain dense
index is enough - that is the default.

The moment an id escapes a synchronous call (a gateway sends an order to the
engine, then later sends a `cancel` referencing that id over an async boundary),
two stale-id hazards appear:

1. the order was **closed** and its slot freed - a late `cancel` must be a no-op, not a match;
2. the slot was **recycled** for a *different* order - a late `cancel` must not hit the wrong order.

The fix is a **generation**: pack a per-slot counter into the id and bump it on
every reuse, so a stale id (old generation) never aliases the slot's current
occupant.

```cpp
using GenBook = OrderBook<1 << 15, 1 << 10, GenerationalId<1 << 15, 24>>;
```

### How it works

- **The generation lives in the id; the pool just bumps it.** The free list
  stores full ids, generation included. `release(id)` pushes the id back
  verbatim; `acquire()` pops it and calls `bumpGeneration()`. The id type owns
  the counter and how it wraps; the pool only advances it on reuse. Nothing else
  touches the counter.
- **The book only stores and compares.** A slot stores its generation only when
  the id has one; `remove(id)` rejects when `node.gen != id.generation()`.
- **One comparison covers all three cases.** `closed` (generation already moved
  on), `recycled` (moved on again), and `live` (matches) are all distinguished by
  the same `==` - no separate "alive/dead" status enum.

### Zero-cost by default

`RawId<Size>` (the default) is a thin wrapper whose `generation()` is a constant
`0` and `bumpGeneration()` is a no-op. The generation field in each slot is a
`[[no_unique_address]]` empty type, so the non-generational book is
byte-for-byte identical to a plain index book - liveness is still just
`qty != 0`.

| Id type | `Node` (32K/1K) | `Node` (131K/16K) | order id |
|---|---|---|---|
| `RawId` (default) | 12 B | 16 B | 2 / 4 B |
| `GenerationalId` (default 32 gen bits) | 16 B | 20 B | 8 B |

Add +4 B per slot when self-trade prevention is enabled (`Participant = uint32_t`).
You pay for the generation counter only when you actually ask for ABA protection.

### Why plain LIFO + a wide generation

LIFO recycling is deliberate: it reuses the most recently freed slots, keeping
the hot working set in cache. Its only downside is that those same hot slots get
their generation bumped fastest, so a narrow counter would wrap sooner. Rather
than slow that down - a LIFO→FIFO hybrid adds a branch on the hot path to work
around a problem you can instead avoid - the counter is made wide.

The id's storage is `uint32_t` when `index bits + generation bits ≤ 32`, else
`uint64_t`. For a 16-bit index, up to 16 generation bits are "free" (the id stays
`uint32_t`); 17-32 generation bits double the id to `uint64_t`. `GenerationalId`
defaults to **32 generation bits ≈ 4.3B reuses per slot** before wrapping, and
the width is a template parameter you can raise.

## Data structures

- **Direct-indexed price levels.** `levels_[slot]` - O(1), no hashing, no
  collisions, no load-factor degradation. The trade-off is explicit: you reserve
  memory for the entire configured window up front.
- **Hierarchical (multi-level) bitmap** for best-bid/best-ask discovery. Each
  level is a 64:1 summary of the level below, down to a single word, so
  `min()`/`max()`/`findFirst()`/`findLast()` walk a constant number of levels
  regardless of how wide the occupied range is. The same class of trick used in
  the Linux O(1) scheduler's priority bitmaps and in buddy-allocator free-lists.
- **Free-list order-slot recycling.** Order ids are indices into a flat node
  array; released ids go onto a stack and are reused LIFO, keeping the working
  set of recently-touched nodes hot in cache.
- **Byte-conscious layout.** Index types (`OrderIdx`, `PriceIdx`) resolve to
  `uint16_t` or `uint32_t` at compile time depending on the configured capacity
  (`IndexType<Size>`).

## Determinism and sequence numbers

Every mutating operation is assigned a monotonic `Sequence`, stamped on the
result ack and every fill. Ids are recycled LIFO and there is no hashing,
randomness or clock, so replaying the same operations reproduces the same ids,
fills and state.

- `snapshot()` / `restore()` / `snapshotInto()` capture the full state (nodes,
  levels, bitmap, id pool, window base, next sequence) for save/restore and
  state comparison.
- Replaying the same operations on a fresh engine reproduces the same ids, fills
  and book state, so two engines fed identical input can be diffed
  state-for-state.
- `reduce()` and `clear()` sit outside that stream: `clear()` resets the
  sequence (fresh state), and `reduce()` is a feed-driven execute-by-id
  primitive.

## Safety model

FlatBook splits its API into two categories by how they handle bad input:

- **Validated** - always safe, no preconditions. `OrderBook::insert()` and
  `MatchingEngine::addOrder()` / `modify()` reject invalid input (zero quantity,
  out-of-window price, bad order-type/TIF combinations) and return
  `INVALID_ORDER` / `Result::Rejected` instead of misbehaving.
- **Precondition-guarded** - cheap accessors and trusted internals that assume a
  valid id / in-window price: `qty()`, `priceOf()`, `sideOf()`, `participantOf()`,
  `order()`, `level()`, `head()`, `volume()`, `nextOf()`, `IdPool::release()`,
  `Bitmap::set/reset/contains` and the engine's private `reduce()`. These are
  checked with `assert()` and compile out under `NDEBUG`.

In a Release build (`NDEBUG`), violating a precondition is undefined behavior
(typically an out-of-bounds read/write). The preconditions hold by construction
when you follow the intended flow: keep the id returned by `insert()`/`addOrder()`
and check `isLive()` before cancel/modify; keep prices inside the window. To
catch violations in development, build with the sanitizer profile
(`./dev.sh sanitize`), which keeps `assert` enabled and adds ASan/UBSan.
