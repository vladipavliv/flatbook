#pragma once

#include <cstdint>

#include "MatchingEngine.hpp"

namespace flatbook {

/**
 * @brief Market-protection wrapper: price band + volatility halt.
 *
 * Wraps a MatchingEngine without touching its hot path. It maintains a
 * reference price (the last trade) and rejects limit orders whose price falls
 * outside `[reference - band, reference + band]`; a trade that moves the
 * reference by more than `maxMove` halts the market (all further orders are
 * rejected until `resume()`).
 *
 * Rejections happen before the engine is reached, so they do not consume a
 * sequence; a band rejection carries `Reason::PriceOutOfWindow`, a halted
 * rejection carries `Reason::None` (query `halted()` for the state).
 *
 * @tparam Engine A MatchingEngine type.
 */
template <typename Engine>
class CircuitBreaker {
public:
  using Price = typename Engine::Price;
  using AddResult = typename Engine::AddResult;
  using Order = typename Engine::Order;
  using Fill = typename Engine::Fill;

  CircuitBreaker(Engine &engine, Price reference, Price band, Price maxMove) noexcept
      : engine_{engine}, reference_{reference}, band_{band}, maxMove_{maxMove} {}

  /// @return true when the market is halted (manual or volatility).
  [[nodiscard]]
  bool halted() const noexcept {
    return halted_;
  }

  /// @return The current reference (last trade) price.
  [[nodiscard]]
  Price reference() const noexcept {
    return reference_;
  }

  /// @return true when @p price is inside [reference - band, reference + band].
  [[nodiscard]]
  bool inBand(Price price) const noexcept {
    return price >= reference_ - band_ && price <= reference_ + band_;
  }

  /// Halt the market (reject every further order).
  void halt() noexcept {
    halted_ = true;
  }

  /// Resume from a halt, establishing a new reference price.
  void resume(Price newReference) noexcept {
    halted_ = false;
    reference_ = newReference;
  }

  /**
   * @brief Add an order, guarded by the band and the halt state.
   *
   * @param onFill Fill sink; called for every fill (after the reference price
   *               and halt state have been updated).
   */
  template <typename OnFill>
  AddResult addOrder(const Order &order, OnFill &&onFill) {
    if (halted_) {
      return reject(order);
    }
    if (order.type == Engine::OrderType::Limit && !inBand(order.price)) {
      return reject(order);
    }
    return engine_.addOrder(order, [&](const Fill &f) {
      checkMove(f.price);
      reference_ = f.price;
      onFill(f);
    });
  }

private:
  AddResult reject(const Order &order) const noexcept {
    AddResult r;
    r.result = Engine::Result::Rejected;
    r.reason = halted_ ? Engine::Reason::None : Engine::Reason::PriceOutOfWindow;
    r.orderId = Engine::INVALID_ORDER;
    r.remaining = order.quantity;
    r.seq = 0; // rejected before the engine: no sequence is consumed
    return r;
  }

  void checkMove(Price price) noexcept {
    const Price move = price >= reference_ ? price - reference_ : reference_ - price;
    if (move > maxMove_) {
      halted_ = true;
    }
  }

  Engine &engine_;
  Price reference_;
  Price band_;
  Price maxMove_;
  bool halted_{false};
};

} // namespace flatbook
