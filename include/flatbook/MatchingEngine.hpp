#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "Concepts.hpp"
#include "Id.hpp"
#include "OrderBook.hpp"
#include "Types.hpp"

namespace flatbook {

/**
 * @brief Matching layer on top of @ref OrderBook.
 *
 * Implements price-time crossing, order submission with order types and
 * time-in-force, cancellation/modification and fill reporting. The underlying
 * OrderBook is a pure data structure; all crossing policy lives here.
 *
 * Fills are reported through a caller-supplied callback (default: discard),
 * which keeps the engine allocation-free on the hot path.
 *
 * Every order submission (add and modify) is assigned a monotonically
 * increasing `Sequence`, which is stamped on the resulting ack and every fill,
 * giving a deterministic order of submission and execution events for replay.
 * `cancelOrder` and `rebase` are assigned sequences too (via their result
 * structs), so every mutating operation carries a sequence and can be replayed
 * deterministically.
 *
 * Self-trade prevention is optional: when the book's `Participant` type is not
 * `void`, each order carries a participant key. A key of `0` disables STP (the
 * order matches anything); a non-zero key groups orders into a participant
 * whose orders never cross each other. The policy is cancel-newest: if an
 * aggressor's unfilled remainder would cross its own resting orders, the
 * remainder is cancelled instead of rested, so the book is never crossed.
 *
 * @note The self-trade skip re-walks a level's FIFO head on each fill, so with
 * STP enabled matching is O(k) in the number of orders at a level, not O(1).
 * `postOnly` checks crossing against the whole opposite side without skipping
 * the caller's own orders, so a post-only order that would only cross its own
 * resting orders is rejected rather than rested.
 *
 * @tparam Book A conforming order book type (see @ref OrderBook).
 */
template <OrderBookLike Book>
class MatchingEngine {
public:
  using OrderIdx = typename Book::OrderIdx;
  using Price = typename Book::Price;
  using Side = typename Book::Side;

  /// Monotonic per-order sequence number.
  using Sequence = std::uint64_t;

  /// Whether self-trade prevention is enabled (participant keys stored).
  static constexpr bool kHasParticipants = Book::kHasParticipants;
  /// Order-slot capacity.
  static constexpr std::uint32_t kSize = Book::kSize;
  /// Number of configured price levels (the window width).
  static constexpr std::uint32_t kPriceLevels = Book::kPriceLevels;
  /// Participant key type (uint32_t placeholder when disabled).
  using ParticipantType = typename Book::ParticipantType;

  static constexpr OrderIdx INVALID_ORDER = Book::INVALID_ORDER;
  static constexpr Price INVALID_PRICE = Book::INVALID_PRICE;

  /// Order kind: a price-resting limit order or a marketable order.
  enum class OrderType : std::uint8_t { Limit, Market };

  /// How an unfilled remainder is handled.
  enum class TimeInForce : std::uint8_t {
    GTC, ///< Good-til-cancelled: match, then rest the remainder.
    IOC, ///< Immediate-or-cancel: match, then discard the remainder.
    FOK, ///< Fill-or-kill: fill entirely or reject (nothing partial).
  };

  /**
   * @brief An order submission request.
   *
   * `postOnly` is honored only for Limit + GTC and means the order must rest
   * without ever crossing the opposite side; any other combination with
   * postOnly is rejected.
   *
   * `participant` exists only when the book's Participant type is non-void; it
   * is the self-trade-prevention key. `0` means "no STP" (the order matches
   * anything); a non-zero key groups orders into a participant whose orders
   * never cross each other.
   */
  struct Order {
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    bool postOnly{false};
    Price price{};
    Quantity quantity{};
    Side side{};
    FLATBOOK_NO_UNIQUE_ADDRESS std::conditional_t<kHasParticipants, ParticipantType, NoParticipant>
        participant{};
  };

  /// Outcome of an order submission.
  enum class Result : std::uint8_t {
    Filled,    ///< Fully matched, nothing left.
    Resting,   ///< Remainder is resting (limit GTC / post-only).
    Cancelled, ///< Unfilled remainder discarded (IOC, or market with leftover).
    Rejected,  ///< Nothing executed (invalid, FOK impossible, post-only would cross).
  };

  /// Why an order was rejected or cancelled (adds detail beyond `Result`).
  enum class Reason : std::uint8_t {
    None,               ///< No special reason (filled, resting, or a normal remainder discard).
    InvalidQuantity,    ///< Zero quantity.
    InvalidPostOnly,    ///< postOnly combined with market / non-GTC.
    PriceOutOfWindow,   ///< Limit price outside the configured price window.
    PostOnlyWouldCross, ///< Post-only order would have crossed.
    FokInsufficientLiquidity, ///< Fill-or-kill could not fill entirely.
    BookFull,                 ///< No free order slot.
    StpCancelNewest,          ///< Remainder would self-trade (cancelled by cancel-newest STP).
    UnknownOrder,             ///< Cancel/modify referenced a stale or unknown order id.
    SideMismatch,             ///< Modify replacement side differs from the original.
    UnsupportedModify, ///< Modify replacement is market / non-GTC (only limit GTC replaceable).
  };

  /**
   * @brief One executed match between an aggressor and a resting order.
   */
  struct Fill {
    OrderIdx orderId{}; ///< Resting order id (already recycled if fully filled).
    Price price{};      ///< Execution price (the resting order's price).
    Quantity quantity{};
    Side side{};    ///< Resting order side.
    Sequence seq{}; ///< Sequence of the aggressor order that caused this fill.
  };

  /**
   * @brief Result of addOrder().
   */
  struct AddResult {
    Result result{Result::Rejected};
    OrderIdx orderId{INVALID_ORDER}; ///< Resting order id, valid only when Resting.
    Quantity remaining{};            ///< Discarded (unfilled, unrested) quantity; 0 when Resting.
    Sequence seq{};                  ///< Sequence assigned to this order.
    Reason reason{Reason::None};     ///< Why Rejected/Cancelled (None otherwise).

    /// @return true when the order (or its remainder) is resting.
    [[nodiscard]]
    bool resting() const noexcept {
      return orderId != INVALID_ORDER;
    }
  };

  /// Default fill sink that discards every fill.
  struct NoopFill {
    constexpr void operator()(const Fill &) const noexcept {}
  };

  /**
   * @brief Submit an order.
   *
   * Fills are delivered to @p onFill in execution order; the callback must not
   * throw (the engine is noexcept by contract).
   *
   * @tparam OnFill Callable accepting a `const Fill&`.
   * @param order  Order request.
   * @param onFill Fill sink (defaults to discard).
   * @return Outcome and, for Resting, the resting order id.
   */
  template <typename OnFill = NoopFill>
  [[nodiscard]]
  AddResult addOrder(const Order &order, OnFill &&onFill = NoopFill{}) noexcept {
    const Quantity quantity = order.quantity;
    const Sequence seq = nextSeq_++;

    if (quantity == 0) {
      return {Result::Rejected, INVALID_ORDER, 0, seq, Reason::InvalidQuantity};
    }

    // PostOnly is a resting-limit concept: never cross, always rest.
    if (order.postOnly && (order.type != OrderType::Limit || order.tif != TimeInForce::GTC)) {
      return {Result::Rejected, INVALID_ORDER, quantity, seq, Reason::InvalidPostOnly};
    }

    if (order.type == OrderType::Limit && !book_.isPriceInWindow(order.price)) {
      return {Result::Rejected, INVALID_ORDER, quantity, seq, Reason::PriceOutOfWindow};
    }

    if (order.postOnly) {
      if (crossable(order)) {
        return {Result::Rejected, INVALID_ORDER, quantity, seq, Reason::PostOnlyWouldCross};
      }
      const OrderIdx id = rest(order, quantity);
      if (id == INVALID_ORDER) {
        return {Result::Rejected, INVALID_ORDER, quantity, seq, Reason::BookFull};
      }
      return {Result::Resting, id, 0, seq};
    }

    // FOK: all or nothing. Verified read-only before mutating the book.
    if (order.tif == TimeInForce::FOK && !canFillFully(order)) {
      return {Result::Rejected, INVALID_ORDER, quantity, seq, Reason::FokInsufficientLiquidity};
    }

    bool skippedSelf = false;
    Quantity remaining = match(order, onFill, seq, participantOf(order), skippedSelf);

    if (remaining == 0) {
      return {Result::Filled, INVALID_ORDER, 0, seq};
    }

    switch (order.tif) {
    case TimeInForce::GTC:
      if (order.type == OrderType::Limit) {
        if constexpr (kHasParticipants) {
          // cancel-newest: the remainder would self-trade, so cancel it instead
          // of resting it (resting would cross the aggressor's own orders).
          if (skippedSelf) {
            return {Result::Cancelled, INVALID_ORDER, remaining, seq, Reason::StpCancelNewest};
          }
        }
        const OrderIdx id = rest(order, remaining);
        if (id == INVALID_ORDER) {
          // Unreachable when any fill already happened (a full fill frees a
          // slot), so this is the "book full, nothing executed" case.
          return {Result::Rejected, INVALID_ORDER, remaining, seq, Reason::BookFull};
        }
        return {Result::Resting, id, 0, seq};
      }
      return {Result::Cancelled, INVALID_ORDER, remaining, seq};

    case TimeInForce::IOC:
      return {Result::Cancelled, INVALID_ORDER, remaining, seq};

    case TimeInForce::FOK:
      return {Result::Rejected, INVALID_ORDER, remaining, seq, Reason::FokInsufficientLiquidity};
    }

    return {Result::Rejected, INVALID_ORDER, remaining, seq};
  }

  /// Outcome of a cancel request.
  struct CancelResult {
    bool cancelled{false};       ///< true when the id was live and was removed.
    Sequence seq{0};             ///< Sequence assigned to this cancel.
    Reason reason{Reason::None}; ///< UnknownOrder when the id was stale.
  };

  /**
   * @brief Cancel a resting order.
   * @param id Order id returned by addOrder() (Resting result).
   * @return cancelled = true if removed, false if the id is not live (or
   *         stale); seq = the sequence assigned to this cancel (always
   *         consumed, even for a stale id, so replay stays deterministic).
   */
  [[nodiscard]]
  CancelResult cancelOrder(OrderIdx id) noexcept {
    const Sequence seq = nextSeq_++;
    const bool ok = book_.remove(id);
    return {ok, seq, ok ? Reason::None : Reason::UnknownOrder};
  }

  /**
   * @brief Reduce (execute against) a specific resting order by id.
   *
   * Feed-driven "execute by id": unlike addOrder, this reduces a named order
   * directly, bypassing price-time priority. Used to replay market-data feeds
   * (ITCH/LOBSTER) that reference the exact order being executed or partially
   * cancelled. Reducing to zero removes the order and recycles its id.
   *
   * @param id       Live order id.
   * @param quantity Quantity to reduce; @pre quantity <= the order's remaining
   *                 quantity (asserted; unchecked under NDEBUG).
   * @return true if the order was live (and reduced); false if stale.
   */
  [[nodiscard]]
  bool reduce(OrderIdx id, Quantity quantity) noexcept {
    if (!book_.isLive(id)) {
      return false;
    }
    book_.reduce(id, quantity);
    return true;
  }

  /**
   * @brief Feed-driven "insert without matching": rest an order at @p price
   * without crossing the opposite side.
   *
   * Replaying a market-data feed (ITCH/LOBSTER) must build the book exactly as
   * the feed does - apply submissions, executions and cancellations by id - and
   * NOT match price-time (the feed already records the executions). This is the
   * feed-side primitive for that; it does not consume a sequence.
   *
   * @return The resting order id, or INVALID_ORDER if the price is outside the
   *         window or the book is full.
   */
  [[nodiscard]]
  OrderIdx restOrder(Price price, Quantity quantity, Side side,
                     ParticipantType participant = ParticipantType{}) noexcept {
    return book_.insert(price, quantity, side, participant);
  }

  /**
   * @brief Cancel an order and submit a replacement (cancel/replace).
   *
   * The replacement must be a Limit + GTC on the same side as the original
   * order; this is validated (together with quantity/price range) before any
   * mutation, so an invalid replacement leaves the original order untouched.
   * The replacement is submitted as a fresh order and loses time priority.
   *
   * The replacement's participant is ignored: the key is inherited from the
   * original order, so a modify cannot change ownership. With STP, the
   * replacement can still be cancelled by cancel-newest (if it would
   * self-trade), which effectively cancels the order - the original is already
   * removed.
   *
   * @param id          Order id to replace (must be live; generation-checked).
   * @param replacement New order to submit (Limit + GTC, same side).
   * @param onFill      Fill sink for the replacement's matches.
   * @return The replacement's outcome; Rejected if @p id was not live, the
   *         replacement is invalid, or the sides/type differ.
   */
  template <typename OnFill = NoopFill>
  [[nodiscard]]
  AddResult modify(OrderIdx id, const Order &replacement, OnFill &&onFill = NoopFill{}) noexcept {
    const Sequence seq = nextSeq_++; // a modify consumes a sequence even if rejected
    if (!book_.isLive(id)) {
      return {Result::Rejected, INVALID_ORDER, replacement.quantity, seq, Reason::UnknownOrder};
    }
    if (replacement.type != OrderType::Limit || replacement.tif != TimeInForce::GTC) {
      return {Result::Rejected, INVALID_ORDER, replacement.quantity, seq,
              Reason::UnsupportedModify};
    }
    if (book_.sideOf(id) != replacement.side) {
      return {Result::Rejected, INVALID_ORDER, replacement.quantity, seq, Reason::SideMismatch};
    }
    Order repl = replacement;
    if constexpr (kHasParticipants) {
      repl.participant = book_.participantOf(id); // inherit ownership, before remove
    }
    const Reason rejectReason = wouldReject(repl);
    if (rejectReason != Reason::None) {
      return {Result::Rejected, INVALID_ORDER, repl.quantity, seq, rejectReason};
    }
    (void)book_.remove(id);        // id is live, so this succeeds
    return addOrder(repl, onFill); // the replacement gets its own sequence
  }

  /// Remove all orders and reset the engine (and sequence) to its empty state.
  ///
  /// @note This invalidates all outstanding order ids (see OrderBook::clear())
  /// and resets the sequence counter, so sequence numbers are not unique
  /// across a clear.
  void clear() noexcept {
    book_.clear();
    nextSeq_ = 1;
  }

  /// @return Number of currently live orders.
  [[nodiscard]]
  std::size_t openOrders() const noexcept {
    return book_.openOrders();
  }

  /// @return Best bid price, or INVALID_PRICE when no bids rest.
  [[nodiscard]]
  Price bestBid() const noexcept {
    return book_.bestBid();
  }

  /// @return Best ask price, or INVALID_PRICE when no asks rest.
  [[nodiscard]]
  Price bestAsk() const noexcept {
    return book_.bestAsk();
  }

  /// @return The logical price mapped to slot 0 (the window's lower edge).
  [[nodiscard]]
  Price base() const noexcept {
    return book_.base();
  }

  /// @return true when @p price maps to a slot inside the current window.
  [[nodiscard]]
  bool isPriceInWindow(Price price) const noexcept {
    return book_.isPriceInWindow(price);
  }

  /// Outcome of a rebase request.
  struct RebaseResult {
    std::vector<OrderIdx> cancelled{}; ///< Orders removed because they left the window.
    Sequence seq{0};                   ///< Sequence assigned to this rebase.
  };

  /**
   * @brief Move the price window so that slot 0 maps to @p newBase.
   *
   * Delegates to the underlying book (see OrderBook::rebase). Orders that fell
   * outside the new window are removed and their ids returned in `cancelled`;
   * the engine does not emit a cancel event for them itself, so the caller is
   * responsible for reporting the cancellations. The rebase is assigned a
   * sequence (stamped in `seq`) so it can be replayed deterministically.
   *
   * @return The cancelled order ids and the sequence assigned to this rebase.
   */
  [[nodiscard]]
  RebaseResult rebase(Price newBase) noexcept {
    const Sequence seq = nextSeq_++;
    return {book_.rebase(newBase), seq};
  }

  /// Full state snapshot of the engine, for serialization / restore.
  struct Snapshot {
    typename Book::Snapshot book{};
    Sequence nextSeq{1};
  };

  /// Fill @p out with the engine's full state (no copy; use for large books
  /// where a stack Snapshot would overflow).
  void snapshotInto(Snapshot &out) const noexcept {
    book_.snapshotInto(out.book);
    out.nextSeq = nextSeq_;
  }

  /// @return A copy of the engine's full state.
  [[nodiscard]]
  Snapshot snapshot() const noexcept {
    Snapshot s;
    snapshotInto(s);
    return s;
  }

  /// Restore the engine's full state.
  void restore(const Snapshot &s) noexcept {
    book_.restore(s.book);
    nextSeq_ = s.nextSeq;
  }

  /// @return The last sequence number assigned (0 if none yet).
  [[nodiscard]]
  Sequence lastSequence() const noexcept {
    return nextSeq_ - 1;
  }

  /// @return Mutable access to the underlying book.
  [[nodiscard]]
  Book &book() noexcept {
    return book_;
  }

  /// @return Read-only access to the underlying book.
  [[nodiscard]]
  const Book &book() const noexcept {
    return book_;
  }

private:
  /// Rest the order's remainder into the book, threading the participant key.
  [[nodiscard]]
  OrderIdx rest(const Order &order, Quantity quantity) noexcept {
    if constexpr (kHasParticipants) {
      return book_.insert(order.price, quantity, order.side, order.participant);
    } else {
      return book_.insert(order.price, quantity, order.side);
    }
  }

  /// @return The order's participant key (0 when disabled).
  [[nodiscard]]
  ParticipantType participantOf(const Order &order) const noexcept {
    if constexpr (kHasParticipants) {
      return order.participant;
    } else {
      return 0;
    }
  }

  /// Whether the order would be rejected for a reason checkable before any
  /// book mutation (static constraints plus post-only/FOK pre-checks).
  [[nodiscard]]
  /// @return The reason the order would be rejected, or Reason::None if it is
  ///         acceptable. Used by modify() to report the exact failure.
  [[nodiscard]]
  Reason wouldReject(const Order &order) const noexcept {
    if (order.quantity == 0) {
      return Reason::InvalidQuantity;
    }
    if (order.postOnly && (order.type != OrderType::Limit || order.tif != TimeInForce::GTC)) {
      return Reason::InvalidPostOnly;
    }
    if (order.type == OrderType::Limit && !book_.isPriceInWindow(order.price)) {
      return Reason::PriceOutOfWindow;
    }
    if (order.postOnly && crossable(order)) {
      return Reason::PostOnlyWouldCross;
    }
    if (order.tif == TimeInForce::FOK && !canFillFully(order)) {
      return Reason::FokInsufficientLiquidity;
    }
    return Reason::None;
  }

  /// @return Total quantity at a level matchable by @p aggressor (excludes self).
  [[nodiscard]]
  Volume matchableVolume(Price price, Side side, ParticipantType aggressor) const noexcept {
    if constexpr (!kHasParticipants) {
      return book_.volume(price, side);
    } else {
      Volume v = 0;
      for (OrderIdx id = book_.head(price, side); id != INVALID_ORDER; id = book_.nextOf(id)) {
        if (aggressor == 0 || book_.participantOf(id) != aggressor) {
          v += book_.qty(id);
        }
      }
      return v;
    }
  }

  /// Whether this order would immediately cross the opposite side.
  [[nodiscard]]
  bool crossable(const Order &order) const noexcept {
    if (order.side == Side::Buy) {
      const Price ask = book_.bestAsk();
      return ask != INVALID_PRICE && ask <= order.price;
    }
    const Price bid = book_.bestBid();
    return bid != INVALID_PRICE && bid >= order.price;
  }

  /// Walk the price levels an order crosses, in matching order, calling
  /// `visit(price)` at each. `visit` returns true to stop the walk (e.g. when
  /// the aggressor is fully matched). The resting side is derived from the
  /// order's own side.
  template <typename Visit>
  void walkCrossing(const Order &order, Visit &&visit) const noexcept {
    if (order.side == Side::Buy) {
      Price price = book_.bestAsk();
      while (price != INVALID_PRICE) {
        if (order.type == OrderType::Limit && price > order.price) {
          break;
        }
        if (visit(price)) {
          break;
        }
        price = book_.nextLevel(price + 1, Side::Sell);
      }
    } else {
      Price price = book_.bestBid();
      while (price != INVALID_PRICE) {
        if (order.type == OrderType::Limit && price < order.price) {
          break;
        }
        if (visit(price)) {
          break;
        }
        price = book_.nextLevel(price - 1, Side::Buy);
      }
    }
  }

  /// Whether the book holds enough non-self liquidity to fully fill the order now.
  [[nodiscard]]
  bool canFillFully(const Order &order) const noexcept {
    const Quantity need = order.quantity;
    const ParticipantType aggressor = participantOf(order);
    const Side restSide = order.side == Side::Buy ? Side::Sell : Side::Buy;
    Volume available = 0;

    walkCrossing(order, [&](Price price) {
      available += matchableVolume(price, restSide, aggressor);
      return available >= need;
    });

    return available >= need;
  }

  /// Match the order against the opposite side in price-time order, reporting
  /// fills. Returns the unfilled remainder and sets @p skippedSelf when the
  /// aggressor's participant caused resting orders to be skipped (STP).
  template <typename OnFill>
  [[nodiscard]]
  Quantity match(const Order &order, OnFill &onFill, Sequence seq, ParticipantType aggressor,
                 bool &skippedSelf) noexcept {
    Quantity remaining = order.quantity;
    const Side restSide = order.side == Side::Buy ? Side::Sell : Side::Buy;

    walkCrossing(order, [&](Price price) {
      return matchLevel(price, restSide, remaining, onFill, seq, aggressor,
                        skippedSelf); // true = fully matched
    });

    return remaining;
  }

  /// Consume orders at one level in FIFO order until @p remaining drains,
  /// skipping resting orders belonging to the aggressor's participant (STP).
  /// @return true if the aggressor fully drained; false if the level has no
  ///         matchable (non-self) liquidity left and @p remaining > 0.
  template <typename OnFill>
  bool matchLevel(Price price, Side side, Quantity &remaining, OnFill &onFill, Sequence seq,
                  ParticipantType aggressor, bool &skippedSelf) noexcept {
    OrderIdx id = book_.head(price, side);
    while (remaining != 0) {
      if constexpr (kHasParticipants) {
        while (id != INVALID_ORDER && aggressor != 0 && book_.participantOf(id) == aggressor) {
          skippedSelf = true;
          id = book_.nextOf(id);
        }
      }

      if (id == INVALID_ORDER) {
        return false;
      }

      // Capture next before reduce: a full fill recycles the slot, so id may
      // no longer be valid afterwards. Continuing from next avoids re-walking
      // the level head (and re-skipping self orders) on every iteration.
      const OrderIdx next = book_.nextOf(id);
      const Quantity fill = std::min(remaining, book_.qty(id));

      book_.reduce(id, fill);
      remaining -= fill;
      onFill(Fill{id, price, fill, side, seq});
      id = next;
    }
    return true;
  }

private:
  // --- State ---
  Book book_;           ///< the price-time order book
  Sequence nextSeq_{1}; ///< next sequence number to assign
};

} // namespace flatbook
