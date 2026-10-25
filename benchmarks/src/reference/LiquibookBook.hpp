#pragma once

#include <cstdint>
#include <deque>

#include <book/order_book.h>

namespace flatbook {

// Adapter that drives Liquibook (a callback-based matching engine) through the
// same addLimit/addMarket surface as the other reference books. Liquibook needs
// a stable Order object per order, so orders live in a std::deque (stable
// addresses). It has no resting order id, so addLimit returns a non-zero dummy;
// the book's depth is queried via bestBid/bestAsk/volume for validation.
struct LiquibookOrder {
  bool buy{false};
  liquibook::book::Price price_{0};
  liquibook::book::Quantity qty_{0};

  bool is_buy() const { return buy; }
  liquibook::book::Price price() const { return price_; }
  liquibook::book::Quantity order_qty() const { return qty_; }
  liquibook::book::Price stop_price() const { return 0; }
};

class LiquibookBook {
public:
  std::uint64_t addLimit(std::uint32_t price, std::uint32_t qty, bool buy) {
    orders_.push_back(LiquibookOrder{buy, static_cast<liquibook::book::Price>(price), qty});
    (void)book_.add(&orders_.back());
    return 1; // Liquibook has no resting order id
  }

  std::uint64_t addMarket(std::uint32_t qty, bool buy) {
    orders_.push_back(
        LiquibookOrder{buy, liquibook::book::MARKET_ORDER_PRICE, qty});
    (void)book_.add(&orders_.back());
    return 1;
  }

  std::int64_t bestBid() const {
    const auto &bids = book_.bids();
    if (bids.empty()) {
      return -1;
    }
    return bids.begin()->first.price();
  }

  std::int64_t bestAsk() const {
    const auto &asks = book_.asks();
    if (asks.empty()) {
      return -1;
    }
    return asks.begin()->first.price();
  }

  std::uint32_t volume(std::int64_t price, bool buy) const {
    const auto &side = buy ? book_.bids() : book_.asks();
    auto range = side.equal_range(liquibook::book::ComparablePrice(buy, price));
    std::uint32_t total = 0;
    for (auto it = range.first; it != range.second; ++it) {
      total += static_cast<std::uint32_t>(it->second.open_qty());
    }
    return total;
  }

private:
  liquibook::book::OrderBook<LiquibookOrder *> book_;
  std::deque<LiquibookOrder> orders_;
};

} // namespace flatbook
