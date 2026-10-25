#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

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
 * `cancelOrder` is a synchronous boolean and is not assigned a sequence.
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
  using PriceIdx = typename Book::PriceIdx;
  using Side = typename Book::Side;

  /// Monotonic per-order sequence number.
  using Sequence = std::uint64_t;

  /// Whether self-trade prevention is enabled (participant keys stored).
  static constexpr bool kHasParticipants = Book::kHasParticipants;
  /// Participant key type (uint32_t placeholder when disabled).
  using ParticipantType = typename Book::ParticipantType;

  static constexpr OrderIdx INVALID_ORDER = Book::INVALID_ORDER;
  static constexpr PriceIdx INVALID_PRICE = Book::INVALID_PRICE;

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
    PriceIdx price{};
    Quantity quantity{};
    Side side{};
    FLATBOOK_NO_UNIQUE_ADDRESS std::conditional_t<kHasParticipants, ParticipantType, NoParticipant> participant{};
  };

  /// Outcome of an order submission.
  enum class Result : std::uint8_t {
    Filled,    ///< Fully matched, nothing left.
    Resting,   ///< Remainder is resting (limit GTC / post-only).
    Cancelled, ///< Unfilled remainder discarded (IOC, or market with leftover).
    Rejected,  ///< Nothing executed (invalid, FOK impossible, post-only would cross).
  };

  /**
   * @brief One executed match between an aggressor and a resting order.
   */
  struct Fill {
    OrderIdx orderId{}; ///< Resting order id (already recycled if fully filled).
    PriceIdx price{};   ///< Execution price (the resting order's price).
    Quantity quantity{};
    Side side{};        ///< Resting order side.
    Sequence seq{};     ///< Sequence of the aggressor order that caused this fill.
  };

  /**
   * @brief Result of addOrder().
   */
  struct AddResult {
    Result result{Result::Rejected};
    OrderIdx orderId{INVALID_ORDER}; ///< Resting order id, valid only when Resting.
    Quantity remaining{};            ///< Discarded (unfilled, unrested) quantity; 0 when Resting.
    Sequence seq{};                  ///< Sequence assigned to this order.

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
      return {Result::Rejected, INVALID_ORDER, 0, seq};
    }

    // PostOnly is a resting-limit concept: never cross, always rest.
    if (order.postOnly && (order.type != OrderType::Limit || order.tif != TimeInForce::GTC)) {
      return {Result::Rejected, INVALID_ORDER, quantity, seq};
    }

    if (order.type == OrderType::Limit && order.price >= Book::kPriceLevels) {
      return {Result::Rejected, INVALID_ORDER, quantity, seq};
    }

    if (order.postOnly) {
      if (crossable(order)) {
        return {Result::Rejected, INVALID_ORDER, quantity, seq};
      }
      const OrderIdx id = rest(order, quantity);
      if (id == INVALID_ORDER) {
        return {Result::Rejected, INVALID_ORDER, quantity, seq};
      }
      return {Result::Resting, id, 0, seq};
    }

    // FOK: all or nothing. Verified read-only before mutating the book.
    if (order.tif == TimeInForce::FOK && !canFillFully(order)) {
      return {Result::Rejected, INVALID_ORDER, quantity, seq};
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
            return {Result::Cancelled, INVALID_ORDER, remaining, seq};
          }
        }
        const OrderIdx id = rest(order, remaining);
        if (id == INVALID_ORDER) {
          // Unreachable when any fill already happened (a full fill frees a
          // slot), so this is the "book full, nothing executed" case.
          return {Result::Rejected, INVALID_ORDER, remaining, seq};
        }
        return {Result::Resting, id, 0, seq};
      }
      return {Result::Cancelled, INVALID_ORDER, remaining, seq};

    case TimeInForce::IOC:
      return {Result::Cancelled, INVALID_ORDER, remaining, seq};

    case TimeInForce::FOK:
      return {Result::Rejected, INVALID_ORDER, remaining, seq};
    }

    return {Result::Rejected, INVALID_ORDER, remaining, seq};
  }

  /**
   * @brief Cancel a resting order.
   * @param id Order id returned by addOrder() (Resting result).
   * @return true if cancelled, false if the id is not live (or stale).
   */
  [[nodiscard]]
  bool cancelOrder(OrderIdx id) noexcept {
    return book_.remove(id);
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
    if (!book_.isLive(id) || book_.sideOf(id) != replacement.side ||
        replacement.type != OrderType::Limit || replacement.tif != TimeInForce::GTC) {
      return {Result::Rejected, INVALID_ORDER, replacement.quantity, seq};
    }
    Order repl = replacement;
    if constexpr (kHasParticipants) {
      repl.participant = book_.participantOf(id); // inherit ownership, before remove
    }
    if (wouldReject(repl)) {
      return {Result::Rejected, INVALID_ORDER, repl.quantity, seq};
    }
    (void)book_.remove(id); // id is live, so this succeeds
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
  PriceIdx bestBid() const noexcept {
    return book_.bestBid();
  }

  /// @return Best ask price, or INVALID_PRICE when no asks rest.
  [[nodiscard]]
  PriceIdx bestAsk() const noexcept {
    return book_.bestAsk();
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
  bool wouldReject(const Order &order) const noexcept {
    if (order.quantity == 0) {
      return true;
    }
    if (order.postOnly && (order.type != OrderType::Limit || order.tif != TimeInForce::GTC)) {
      return true;
    }
    if (order.type == OrderType::Limit && order.price >= Book::kPriceLevels) {
      return true;
    }
    if (order.postOnly && crossable(order)) {
      return true;
    }
    if (order.tif == TimeInForce::FOK && !canFillFully(order)) {
      return true;
    }
    return false;
  }

  /// @return Total quantity at a level matchable by @p aggressor (excludes self).
  [[nodiscard]]
  Volume matchableVolume(PriceIdx price, Side side, ParticipantType aggressor) const noexcept {
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
      const PriceIdx ask = book_.bestAsk();
      return ask != INVALID_PRICE && ask <= order.price;
    }
    const PriceIdx bid = book_.bestBid();
    return bid != INVALID_PRICE && bid >= order.price;
  }

  /// Walk the price levels an order crosses, in matching order, calling
  /// `visit(price)` at each. `visit` returns true to stop the walk (e.g. when
  /// the aggressor is fully matched). The resting side is derived from the
  /// order's own side.
  template <typename Visit>
  void walkCrossing(const Order &order, Visit &&visit) const noexcept {
    if (order.side == Side::Buy) {
      PriceIdx price = book_.bestAsk();
      while (price != INVALID_PRICE) {
        if (order.type == OrderType::Limit && price > order.price) {
          break;
        }
        if (visit(price)) {
          break;
        }
        price = book_.nextLevel(static_cast<PriceIdx>(price + 1), Side::Sell);
      }
    } else {
      PriceIdx price = book_.bestBid();
      while (price != INVALID_PRICE) {
        if (order.type == OrderType::Limit && price < order.price) {
          break;
        }
        if (visit(price)) {
          break;
        }
        if (price == 0) {
          break;
        }
        price = book_.nextLevel(static_cast<PriceIdx>(price - 1), Side::Buy);
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

    walkCrossing(order, [&](PriceIdx price) {
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

    walkCrossing(order, [&](PriceIdx price) {
      return matchLevel(price, restSide, remaining, onFill, seq, aggressor, skippedSelf); // true = fully matched
    });

    return remaining;
  }

  /// Consume orders at one level in FIFO order until @p remaining drains,
  /// skipping resting orders belonging to the aggressor's participant (STP).
  /// @return true if the aggressor fully drained; false if the level has no
  ///         matchable (non-self) liquidity left and @p remaining > 0.
  template <typename OnFill>
  bool matchLevel(PriceIdx price, Side side, Quantity &remaining, OnFill &onFill, Sequence seq,
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
  Book book_;
  Sequence nextSeq_{1};
};

} // namespace flatbook
