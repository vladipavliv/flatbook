#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "Bitmap.hpp"
#include "Concepts.hpp"
#include "Id.hpp"
#include "IdPool.hpp"
#include "Types.hpp"

namespace flatbook {

/**
 * @brief Pure price-time priority order book data structure.
 *
 * Maintains ordered price levels with FIFO ordering within each level, and
 * O(1) best-bid/best-ask discovery via a hierarchical bitmap. This class is a
 * pure container: it knows nothing about matching, order types or events -
 * crossing logic lives in @ref MatchingEngine.
 *
 * Two template parameters are optional, each with a zero-cost default:
 * - `Id`: the order id type. `RawId` (default) has no generation; with
 *   `GenerationalId` the node stores a 4-byte generation and `remove()` rejects
 *   stale ids. Generation is owned entirely by the pool.
 * - `Participant`: a per-order self-trade-prevention key. `void` (default)
 *   stores nothing; a non-void type (e.g. `std::uint32_t`) is stored per order
 *   so the engine can skip resting orders belonging to the same participant.
 *
 * Preconditions: the hot-path accessors (`qty`, `priceOf`, `sideOf`,
 * `participantOf`, `order`, `level`, `nextOf`) and the trusted `reduce()` are
 * guarded by `assert()` only and assume a live order id or an in-range price
 * index; under `NDEBUG` (Release) these checks compile out and a violation is
 * undefined behavior. `insert()` is the exception: it validates its inputs and
 * returns INVALID_ORDER instead of asserting.
 *
 * @tparam Size        Maximum number of order slots (capacity is Size - 1,
 *                     since index 0 is reserved as the null sentinel).
 * @tparam PriceLevels Number of distinct price levels (tick indexes 0..PriceLevels-1).
 * @tparam Id          Order id type (RawId<Size> or GenerationalId<Size, GenBits>).
 * @tparam Participant Self-trade-prevention key type, or void to disable.
 */
template <std::uint32_t Size, std::uint32_t PriceLevels,
          OrderIdLike<IndexType<Size>> Id = RawId<Size>, typename Participant = void>
class OrderBook {
  static_assert(Size >= 2, "OrderBook: Size must be at least 2 (index 0 is the null sentinel)");
  static_assert(PriceLevels >= 1, "OrderBook: PriceLevels must be at least 1");

  template <OrderBookLike>
  friend class MatchingEngine; // the engine drives matching via the private reduce()

public:
  /// Caller-facing order id (with optional generation).
  using OrderIdx = Id;
  /// Raw dense slot index used internally (links, array subscripts).
  using SlotIdx = IndexType<Size>;
  /// Price level index (slot): the storage offset within the current window.
  /// Internal: `levels_` and the bitmap are indexed by slot, not by price.
  using PriceIdx = IndexType<PriceLevels>;

  /// Logical price (tick). Unbounded; the window maps it to a slot via
  /// `slot = price - base_`, so the book is not tied to a fixed compile-time
  /// price range. Prices are assumed to stay within `kPriceLevels` of `base_`.
  using Price = std::int64_t;

  /// Whether per-order self-trade-prevention keys are stored.
  static constexpr bool kHasParticipants = !std::is_void_v<Participant>;
  /// Participant key type (uint32_t placeholder when disabled).
  using ParticipantType = std::conditional_t<kHasParticipants, Participant, std::uint32_t>;

  /// Number of order slots (see NULL_SLOT for the reserved index).
  static constexpr std::uint32_t kSize = Size;
  /// Number of configured price levels.
  static constexpr std::uint32_t kPriceLevels = PriceLevels;

  /// Reserved raw index used as the null link sentinel; never issued.
  static constexpr SlotIdx NULL_SLOT = 0;
  /// Id meaning "no order" (insert() failed / capacity exhausted).
  static constexpr OrderIdx INVALID_ORDER = Id::invalid();
  /// Logical price meaning "no such level".
  static constexpr Price INVALID_PRICE = std::numeric_limits<Price>::min();

  /// Order side.
  enum class Side : std::uint8_t { Buy, Sell };

  /** @brief A single order slot, doubly linked within its level.
   *
   * Fields are ordered to minimize padding: 4-byte fields first, then 2-byte,
   * then 1-byte, then the conditional generation/participant keys.
   */
  struct Node {
    Quantity qty{};   ///< Remaining quantity (0 = slot not live).
    SlotIdx next{};   ///< Next order in level (NULL_SLOT if tail).
    SlotIdx prev{};   ///< Previous order in level (NULL_SLOT if head).
    PriceIdx price{}; ///< Price level slot; logical price is `base_ + price`.
    Side side{};      ///< Side of the level this order rests in.
    // Per-order generation for ABA protection; zero-size for non-generational ids.
    FLATBOOK_NO_UNIQUE_ADDRESS std::conditional_t<Id::kHasGeneration, std::uint32_t, NoGeneration> gen{};
    // Per-order participant key for STP; zero-size when disabled.
    FLATBOOK_NO_UNIQUE_ADDRESS std::conditional_t<kHasParticipants, ParticipantType, NoParticipant>
        participant{};
  };

  /** @brief One side of a price level: aggregate volume plus a FIFO queue. */
  struct PriceLevelSide {
    Volume volume{}; ///< Total quantity resting at this level/side.
    SlotIdx head{};  ///< Oldest order (first to match).
    SlotIdx tail{};  ///< Newest order (last inserted).
  };

  /** @brief Both sides of a single price level. */
  struct PriceLevel {
    PriceLevelSide bid{};
    PriceLevelSide ask{};
  };

public:
  OrderBook() = default;

  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;

  OrderBook(OrderBook &&) = delete;
  OrderBook &operator=(OrderBook &&) = delete;

  /**
   * @brief Rest an order at the back of a price level (FIFO).
   * @param price       Logical price (must be inside the current window).
   * @param quantity    Quantity to rest (must be > 0).
   * @param side        Buy or Sell.
   * @param participant Self-trade-prevention key (ignored when disabled).
   * @return The new order id, or INVALID_ORDER on bad input / exhausted capacity.
   */
  [[nodiscard]]
  OrderIdx insert(Price price, Quantity quantity, Side side,
                  ParticipantType participant = ParticipantType{}) noexcept {
    if (quantity == 0 || !isPriceInWindow(price)) {
      return INVALID_ORDER;
    }
    const PriceIdx priceSlot = toSlot(price);

    const OrderIdx id = idPool_.acquire();
    if (id == INVALID_ORDER) {
      return INVALID_ORDER;
    }

    const SlotIdx slot = id.index();
    Node &node = nodes_[slot];
    node.qty = quantity;
    node.price = priceSlot;
    node.next = NULL_SLOT;
    node.prev = NULL_SLOT;
    node.side = side;
    if constexpr (Id::kHasGeneration) {
      node.gen = id.generation();
    }
    if constexpr (kHasParticipants) {
      node.participant = participant;
    }

    PriceLevelSide &level = levelSide(priceSlot, side);

    node.prev = level.tail;
    if (level.tail != NULL_SLOT) {
      nodes_[level.tail].next = slot;
    } else {
      level.head = slot;
      bitmap(side).set(priceSlot);
    }

    level.tail = slot;
    level.volume += quantity;

    return id;
  }

  /**
   * @brief Whether the id refers to a live (resting) order.
   *
   * For generational ids the generation must also match, so a stale id is not
   * considered live.
   */
  [[nodiscard]]
  bool isLive(OrderIdx id) const noexcept {
    const SlotIdx slot = id.index();
    if (slot == NULL_SLOT || slot >= kSize) {
      return false;
    }
    const Node &node = nodes_[slot];
    if (node.qty == 0) {
      return false;
    }
    if constexpr (Id::kHasGeneration) {
      return node.gen == id.generation();
    }
    return true;
  }

  /**
   * @brief Remove a whole live order (cancel).
   *
   * For generational ids the incoming id's generation must match the slot's
   * current generation, so a stale (already-closed or recycled) id is rejected.
   * @param id Order id returned by insert().
   * @return true if removed, false if the id is not live (or stale).
   */
  [[nodiscard]]
  bool remove(OrderIdx id) noexcept {
    if (!isLive(id)) {
      return false;
    }

    const SlotIdx slot = id.index();
    Node &node = nodes_[slot];
    PriceLevelSide &level = levelSide(node.price, node.side);
    level.volume -= node.qty;
    unlink(node, level);

    idPool_.release(id);
    node = {};

    return true;
  }

  /// Remove all orders and reset the book to its empty state.
  ///
  /// @note This invalidates all outstanding order ids: after clear(), an id
  /// issued before the call may alias a newly issued id (ABA protection does
  /// not span a clear).
  void clear() noexcept {
    nodes_.fill({});
    levels_.fill({});

    bidMask_.clear();
    askMask_.clear();

    idPool_.clear();
  }

  /// @return The logical price mapped to slot 0 (the window's lower edge).
  [[nodiscard]]
  Price base() const noexcept {
    return base_;
  }

  /// @return true when @p price maps to a slot inside the current window.
  [[nodiscard]]
  bool isPriceInWindow(Price price) const noexcept {
    return price >= base_ && static_cast<std::uint64_t>(price - base_) < kPriceLevels;
  }

  /**
   * @brief Move the price window so that slot 0 maps to @p newBase.
   *
   * Resting orders whose logical price falls outside the new window
   * `[newBase, newBase + kPriceLevels)` are cancelled (removed) and their ids
   * returned so the caller can report the cancellations. Surviving orders keep
   * their logical price; only their storage slot (and the level arrays and
   * bitmap) is re-indexed.
   *
   * This is a cold, O(open orders + kPriceLevels) operation: the hot path is
   * untouched, but a rebase must not run per order. It allocates only the
   * returned id vector.
   *
   * @return Ids of orders cancelled because they fell outside the new window.
   */
  [[nodiscard]]
  std::vector<OrderIdx> rebase(Price newBase) noexcept {
    std::vector<OrderIdx> cancelled;
    const Price delta = newBase - base_;

    if (delta != 0) {
      // Pass 1: cancel orders that fall outside the new window.
      for (SlotIdx s = 1; s < kSize; ++s) {
        if (nodes_[s].qty == 0) {
          continue;
        }
        const Price logical = base_ + static_cast<Price>(nodes_[s].price);
        if (logical < newBase || logical >= newBase + static_cast<Price>(kPriceLevels)) {
          const OrderIdx id = idOf(s);
          (void)remove(id);
          cancelled.push_back(id);
        }
      }

      // Pass 2: shift survivors and the level arrays to the new base.
      if (delta > 0) {
        const PriceIdx shift = static_cast<PriceIdx>(std::min<Price>(delta, kPriceLevels));
        if (shift == kPriceLevels) {
          levels_.fill({});
        } else {
          std::memmove(&levels_[0], &levels_[shift], sizeof(PriceLevel) * (kPriceLevels - shift));
          std::fill(levels_.begin() + (kPriceLevels - shift), levels_.end(), PriceLevel{});
          for (SlotIdx s = 1; s < kSize; ++s) {
            if (nodes_[s].qty != 0) {
              nodes_[s].price = static_cast<PriceIdx>(nodes_[s].price - shift);
            }
          }
        }
      } else {
        const PriceIdx shift = static_cast<PriceIdx>(std::min<Price>(-delta, kPriceLevels));
        if (shift == kPriceLevels) {
          levels_.fill({});
        } else {
          std::memmove(&levels_[shift], &levels_[0], sizeof(PriceLevel) * (kPriceLevels - shift));
          std::fill(levels_.begin(), levels_.begin() + shift, PriceLevel{});
          for (SlotIdx s = 1; s < kSize; ++s) {
            if (nodes_[s].qty != 0) {
              nodes_[s].price = static_cast<PriceIdx>(nodes_[s].price + shift);
            }
          }
        }
      }
    }

    base_ = newBase;
    rebuildBitmaps();
    return cancelled;
  }

  /// @return Number of currently live orders.
  [[nodiscard]]
  std::size_t openOrders() const noexcept {
    return idPool_.active();
  }

  /// @return Read-only view of an order slot.
  [[nodiscard]]
  const Node &order(OrderIdx id) const noexcept {
    assert(id.index() < kSize);
    return nodes_[id.index()];
  }

  /// @return Read-only view of a price level (both sides).
  [[nodiscard]]
  const PriceLevel &level(Price price) const noexcept {
    assert(isPriceInWindow(price));
    return levels_[toSlot(price)];
  }

  /// @return Remaining quantity of an order.
  [[nodiscard]]
  Quantity qty(OrderIdx id) const noexcept {
    assert(id.index() < kSize);
    return nodes_[id.index()].qty;
  }

  /// @return Logical price level index of an order.
  [[nodiscard]]
  Price priceOf(OrderIdx id) const noexcept {
    assert(id.index() < kSize);
    return base_ + static_cast<Price>(nodes_[id.index()].price);
  }

  /// @return Side of an order.
  [[nodiscard]]
  Side sideOf(OrderIdx id) const noexcept {
    assert(id.index() < kSize);
    return nodes_[id.index()].side;
  }

  /// @return Self-trade-prevention key of an order (0 when participants disabled).
  [[nodiscard]]
  ParticipantType participantOf(OrderIdx id) const noexcept {
    assert(id.index() < kSize);
    if constexpr (kHasParticipants) {
      return nodes_[id.index()].participant;
    } else {
      return 0;
    }
  }

  /**
   * @brief First (oldest) order at a price level/side.
   * @return Order id, or INVALID_ORDER when the level is empty.
   */
  [[nodiscard]]
  OrderIdx head(Price price, Side side) const noexcept {
    const SlotIdx slot = levelSide(toSlot(price), side).head;
    if (slot == NULL_SLOT) {
      return INVALID_ORDER;
    }
    return idOf(slot);
  }

  /**
   * @brief Next order after @p id in its level's FIFO queue.
   * @return Order id, or INVALID_ORDER when @p id is the tail.
   */
  [[nodiscard]]
  OrderIdx nextOf(OrderIdx id) const noexcept {
    assert(id.index() < kSize);
    const SlotIdx next = nodes_[id.index()].next;
    if (next == NULL_SLOT) {
      return INVALID_ORDER;
    }
    return idOf(next);
  }

  /// @return Total resting volume at a price level/side.
  [[nodiscard]]
  Volume volume(Price price, Side side) const noexcept {
    return levelSide(toSlot(price), side).volume;
  }

  /// @return Best bid price, or INVALID_PRICE when no bids rest.
  [[nodiscard]]
  Price bestBid() const noexcept {
    const PriceIdx slot = bidMask_.max();
    if (slot == Bitmap<PriceLevels>::INVALID) {
      return INVALID_PRICE;
    }
    return base_ + static_cast<Price>(slot);
  }

  /// @return Best ask price, or INVALID_PRICE when no asks rest.
  [[nodiscard]]
  Price bestAsk() const noexcept {
    const PriceIdx slot = askMask_.min();
    if (slot == Bitmap<PriceLevels>::INVALID) {
      return INVALID_PRICE;
    }
    return base_ + static_cast<Price>(slot);
  }

  /**
   * @brief Next non-empty level in matching order, at or past @p from (inclusive).
   *
   * Buy side walks down (best bid = highest); Sell side walks up (best ask = lowest).
   * @return Logical price, or INVALID_PRICE when none remains.
   */
  [[nodiscard]]
  Price nextLevel(Price from, Side side) const noexcept {
    if (side == Side::Buy) {
      if (from < base_) {
        return INVALID_PRICE;
      }
      const PriceIdx slot = static_cast<PriceIdx>(std::min<Price>(from - base_, kPriceLevels - 1));
      const PriceIdx found = bidMask_.findLast(slot);
      if (found == Bitmap<PriceLevels>::INVALID) {
        return INVALID_PRICE;
      }
      return base_ + static_cast<Price>(found);
    }
    if (from >= base_ + static_cast<Price>(kPriceLevels)) {
      return INVALID_PRICE;
    }
    const PriceIdx slot = static_cast<PriceIdx>(std::max<Price>(from - base_, 0));
    const PriceIdx found = askMask_.findFirst(slot);
    if (found == Bitmap<PriceLevels>::INVALID) {
      return INVALID_PRICE;
    }
    return base_ + static_cast<Price>(found);
  }

  /// Full state snapshot of the book, for serialization / restore.
  struct Snapshot {
    std::array<Node, Size> nodes{};
    std::array<PriceLevel, PriceLevels> levels{};
    typename Bitmap<PriceLevels>::Snapshot bidMask{};
    typename Bitmap<PriceLevels>::Snapshot askMask{};
    typename IdPool<Size, Id>::Snapshot idPool{};
    Price base{0};
  };

  /// Fill @p out with the book's full state (no copy; use for large books).
  void snapshotInto(Snapshot &out) const noexcept {
    out.nodes = nodes_;
    out.levels = levels_;
    out.bidMask = bidMask_.snapshot();
    out.askMask = askMask_.snapshot();
    out.idPool = idPool_.snapshot();
    out.base = base_;
  }

  /// @return A copy of the book's full state.
  [[nodiscard]]
  Snapshot snapshot() const noexcept {
    Snapshot s;
    snapshotInto(s);
    return s;
  }

  /// Restore the book's full state.
  void restore(const Snapshot &s) noexcept {
    nodes_ = s.nodes;
    levels_ = s.levels;
    bidMask_.restore(s.bidMask);
    askMask_.restore(s.askMask);
    idPool_.restore(s.idPool);
    base_ = s.base;
  }

private:
  /**
   * @brief Reduce a live order's quantity (partial fill).
   *
   * Trusted internal operation driven by the matching engine; no generation
   * check. Unlinks and recycles the slot when the quantity drains to zero.
   * @param id       Order id; @pre refers to a live order.
   * @param quantity Quantity to remove; @pre quantity <= remaining quantity.
   */
  void reduce(OrderIdx id, Quantity quantity) noexcept {
    const SlotIdx slot = id.index();
    assert(slot != NULL_SLOT && slot < kSize);
    assert(quantity <= nodes_[slot].qty);

    Node &node = nodes_[slot];
    node.qty -= quantity;

    PriceLevelSide &level = levelSide(node.price, node.side);
    level.volume -= quantity;

    if (node.qty == 0) {
      unlink(node, level);
      idPool_.release(id);
      node = {};
    }
  }

  /// Reconstruct the full id for a slot from its stored generation.
  [[nodiscard]]
  OrderIdx idOf(SlotIdx slot) const noexcept {
    if constexpr (Id::kHasGeneration) {
      return Id::fromIndexAndGen(slot, nodes_[slot].gen);
    }
    return Id::fromIndex(slot);
  }

  /// @return Bitmap for the given side.
  [[nodiscard]]
  Bitmap<PriceLevels> &bitmap(Side side) noexcept {
    return side == Side::Buy ? bidMask_ : askMask_;
  }

  /// @return Bitmap for the given side.
  [[nodiscard]]
  const Bitmap<PriceLevels> &bitmap(Side side) const noexcept {
    return side == Side::Buy ? bidMask_ : askMask_;
  }

  /// @return Price-level side for the given price/side.
  [[nodiscard]]
  PriceLevelSide &levelSide(PriceIdx price, Side side) noexcept {
    return side == Side::Buy ? levels_[price].bid : levels_[price].ask;
  }

  /// @return Price-level side for the given price/side.
  [[nodiscard]]
  const PriceLevelSide &levelSide(PriceIdx price, Side side) const noexcept {
    return side == Side::Buy ? levels_[price].bid : levels_[price].ask;
  }

  /// Splice a node out of its level's FIFO list, resetting the level bitmap
  /// when the level drains to zero volume.
  void unlink(Node &node, PriceLevelSide &level) noexcept {
    if (node.prev != NULL_SLOT) {
      nodes_[node.prev].next = node.next;
    } else {
      level.head = node.next;
    }

    if (node.next != NULL_SLOT) {
      nodes_[node.next].prev = node.prev;
    } else {
      level.tail = node.prev;
    }

    if (level.volume == 0) {
      bitmap(node.side).reset(node.price);
    }
  }

  /// @return The slot index for a logical price; @pre in window.
  [[nodiscard]]
  PriceIdx toSlot(Price price) const noexcept {
    assert(isPriceInWindow(price));
    return static_cast<PriceIdx>(price - base_);
  }

  /// Rebuild both bitmaps from the level arrays. Cold path (rebase only).
  void rebuildBitmaps() noexcept {
    bidMask_.clear();
    askMask_.clear();
    for (PriceIdx slot = 0; slot < kPriceLevels; ++slot) {
      if (levels_[slot].bid.volume != 0) {
        bidMask_.set(slot);
      }
      if (levels_[slot].ask.volume != 0) {
        askMask_.set(slot);
      }
    }
  }

private:
  // --- Flat storage (direct-indexed) ---
  std::array<Node, Size> nodes_{};                 ///< order slots (slot 0 = sentinel)
  std::array<PriceLevel, PriceLevels> levels_{};   ///< price levels, both sides

  // --- Best-price discovery ---
  Bitmap<PriceLevels> bidMask_;
  Bitmap<PriceLevels> askMask_;

  // --- Order-id recycling ---
  IdPool<Size, Id> idPool_;

  // --- Price window ---
  /// Logical price mapped to slot 0; the window is [base_, base_ + PriceLevels).
  Price base_{0};
};

} // namespace flatbook
