#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <ostream>
#include <random>
#include <vector>

#include "flatbook/MatchingEngine.hpp"

namespace flatbook {
namespace {

// ---------------------------------------------------------------------------
// Naive reference book: std::map price levels + std::deque FIFO queues.
// Monotonic, never-reused order ids make it ABA-safe by construction, so it is
// an independent oracle for FlatBook's matching semantics. It reports the full
// fill stream (price, qty) in matching order and supports GTC/IOC/FOK/post-only.
// ---------------------------------------------------------------------------

struct RefFill {
  std::int64_t price;
  std::uint32_t qty;
  bool operator==(const RefFill &) const = default;
};

std::ostream &operator<<(std::ostream &os, const RefFill &f) {
  return os << "{" << f.price << "," << f.qty << "}";
}

enum class RefTif : std::uint8_t { GTC, IOC, FOK, PostOnly };

class RefBook {
public:
  using Price = std::uint32_t;
  using Qty = std::uint32_t;

  // bids: highest price first (std::greater); asks: lowest price first.
  std::map<Price, std::deque<std::pair<std::uint64_t, Qty>>, std::greater<Price>> bids_;
  std::map<Price, std::deque<std::pair<std::uint64_t, Qty>>> asks_;

  // Returns the resting order id (0 if nothing rests). Fills are appended.
  std::uint64_t addLimit(Price price, Qty qty, bool buy, RefTif tif, std::vector<RefFill> &fills) {
    if (tif == RefTif::PostOnly) {
      if (wouldCross(price, buy)) {
        return 0;
      }
      return rest(price, qty, buy);
    }
    if (tif == RefTif::FOK) {
      if (!canFill(price, qty, buy)) {
        return 0;
      }
      match(price, qty, buy, fills);
      return 0;
    }
    const Qty remaining = match(price, qty, buy, fills);
    if (remaining == 0 || tif == RefTif::IOC) {
      return 0;
    }
    return rest(price, remaining, buy);
  }

  void addMarket(Qty qty, bool buy, std::vector<RefFill> &fills) {
    matchUnlimited(qty, buy, fills);
    // remainder discarded, nothing rests
  }

  bool cancel(std::uint64_t id) {
    if (eraseFrom(bids_, id)) {
      return true;
    }
    return eraseFrom(asks_, id);
  }

  bool hasBid() const { return !bids_.empty(); }
  bool hasAsk() const { return !asks_.empty(); }
  Price bestBid() const { return bids_.begin()->first; }
  Price bestAsk() const { return asks_.begin()->first; }

  Qty volume(Price price, bool buy) const {
    if (buy) {
      const auto it = bids_.find(price);
      if (it == bids_.end()) {
        return 0;
      }
      Qty v = 0;
      for (const auto &[id, q] : it->second) {
        (void)id;
        v += q;
      }
      return v;
    }
    const auto it = asks_.find(price);
    if (it == asks_.end()) {
      return 0;
    }
    Qty v = 0;
    for (const auto &[id, q] : it->second) {
      (void)id;
      v += q;
    }
    return v;
  }

  std::size_t openOrders() const {
    std::size_t n = 0;
    for (const auto &[price, q] : bids_) {
      (void)price;
      n += q.size();
    }
    for (const auto &[price, q] : asks_) {
      (void)price;
      n += q.size();
    }
    return n;
  }

private:
  Qty match(Price limit, Qty qty, bool buy, std::vector<RefFill> &fills) {
    Qty remaining = qty;
    if (buy) {
      while (remaining > 0 && !asks_.empty()) {
        auto it = asks_.begin();
        if (it->first > limit) {
          break;
        }
        consume(it, remaining, fills);
        if (it->second.empty()) {
          asks_.erase(it);
        }
      }
    } else {
      while (remaining > 0 && !bids_.empty()) {
        auto it = bids_.begin();
        if (it->first < limit) {
          break;
        }
        consume(it, remaining, fills);
        if (it->second.empty()) {
          bids_.erase(it);
        }
      }
    }
    return remaining;
  }

  Qty matchUnlimited(Qty qty, bool buy, std::vector<RefFill> &fills) {
    Qty remaining = qty;
    if (buy) {
      while (remaining > 0 && !asks_.empty()) {
        auto it = asks_.begin();
        consume(it, remaining, fills);
        if (it->second.empty()) {
          asks_.erase(it);
        }
      }
    } else {
      while (remaining > 0 && !bids_.empty()) {
        auto it = bids_.begin();
        consume(it, remaining, fills);
        if (it->second.empty()) {
          bids_.erase(it);
        }
      }
    }
    return remaining;
  }

  template <typename It>
  void consume(It it, Qty &remaining, std::vector<RefFill> &fills) {
    const Price price = it->first;
    auto &q = it->second;
    while (remaining > 0 && !q.empty()) {
      Qty &head = q.front().second;
      const Qty fill = std::min(remaining, head);
      head -= fill;
      remaining -= fill;
      fills.push_back({price, fill});
      if (head == 0) {
        q.pop_front();
      }
    }
  }

  bool wouldCross(Price price, bool buy) const {
    if (buy) {
      return !asks_.empty() && asks_.begin()->first <= price;
    }
    return !bids_.empty() && bids_.begin()->first >= price;
  }

  bool canFill(Price price, Qty qty, bool buy) const {
    Volume available = 0;
    if (buy) {
      for (const auto &[p, q] : asks_) {
        if (p > price) {
          break;
        }
        for (const auto &[id, qq] : q) {
          (void)id;
          available += qq;
        }
      }
    } else {
      for (const auto &[p, q] : bids_) {
        if (p < price) {
          break;
        }
        for (const auto &[id, qq] : q) {
          (void)id;
          available += qq;
        }
      }
    }
    return available >= qty;
  }

  std::uint64_t rest(Price price, Qty qty, bool buy) {
    const std::uint64_t id = nextId_++;
    (buy ? bids_[price] : asks_[price]).push_back({id, qty});
    return id;
  }

  template <typename BookMap>
  bool eraseFrom(BookMap &book, std::uint64_t id) {
    for (auto it = book.begin(); it != book.end(); ++it) {
      auto &q = it->second;
      for (auto o = q.begin(); o != q.end(); ++o) {
        if (o->first == id) {
          q.erase(o);
          if (q.empty()) {
            book.erase(it);
          }
          return true;
        }
      }
    }
    return false;
  }

  std::uint64_t nextId_{1};
};

// ---------------------------------------------------------------------------
// Property test: run a deterministic random stream through both books and
// require identical fill streams and identical book state.
// ---------------------------------------------------------------------------

using Book = OrderBook<32768, 128, GenerationalId<32768, 24>>;
using Engine = MatchingEngine<Book>;

void verifyState(const Engine &engine, const RefBook &ref) {
  if (ref.hasBid()) {
    EXPECT_EQ(engine.bestBid(), ref.bestBid());
  } else {
    EXPECT_EQ(engine.bestBid(), Engine::INVALID_PRICE);
  }

  if (ref.hasAsk()) {
    EXPECT_EQ(engine.bestAsk(), ref.bestAsk());
  } else {
    EXPECT_EQ(engine.bestAsk(), Engine::INVALID_PRICE);
  }

  EXPECT_EQ(engine.openOrders(), ref.openOrders());

  for (std::uint32_t p = 0; p < Book::kPriceLevels; ++p) {
    EXPECT_EQ(engine.book().volume(static_cast<Engine::Price>(p), Engine::Side::Buy), ref.volume(p, true)) << "bid level " << p;
    EXPECT_EQ(engine.book().volume(static_cast<Engine::Price>(p), Engine::Side::Sell), ref.volume(p, false)) << "ask level " << p;
  }
}

TEST(OrderBookProperty, MatchesNaiveReference) {
  Engine engine;
  RefBook ref;

  std::vector<std::uint64_t> refIds;
  std::vector<Engine::OrderIdx> engIds;

  std::mt19937_64 rng(0xDEADBEEF);
  std::uniform_int_distribution<std::uint32_t> priceDist(0, Book::kPriceLevels - 1);
  std::uniform_int_distribution<std::uint32_t> qtyDist(1, 20);
  std::bernoulli_distribution sideDist(0.5);
  std::uniform_int_distribution<int> opDist(0, 99);
  std::uniform_int_distribution<int> tifDist(0, 99);

  constexpr int kOps = 20000;
  for (int i = 0; i < kOps; ++i) {
    const bool buy = sideDist(rng);
    const int op = opDist(rng);

    if (op < 65) {
      const std::uint32_t price = priceDist(rng);
      const std::uint32_t qty = qtyDist(rng);
      const int t = tifDist(rng);
      const RefTif tif = t < 75 ? RefTif::GTC : (t < 88 ? RefTif::IOC : (t < 96 ? RefTif::FOK : RefTif::PostOnly));

      Engine::Order order;
      order.type = Engine::OrderType::Limit;
      order.price = static_cast<Engine::Price>(price);
      order.quantity = qty;
      order.side = buy ? Engine::Side::Buy : Engine::Side::Sell;
      switch (tif) {
      case RefTif::GTC:
        order.tif = Engine::TimeInForce::GTC;
        break;
      case RefTif::IOC:
        order.tif = Engine::TimeInForce::IOC;
        break;
      case RefTif::FOK:
        order.tif = Engine::TimeInForce::FOK;
        break;
      case RefTif::PostOnly:
        order.tif = Engine::TimeInForce::GTC;
        order.postOnly = true;
        break;
      }

      std::vector<RefFill> engFills;
      const auto r = engine.addOrder(order, [&](const Engine::Fill &f) {
        engFills.push_back({f.price, f.quantity});
      });
      std::vector<RefFill> refFills;
      const std::uint64_t refId = ref.addLimit(price, qty, buy, tif, refFills);

      EXPECT_EQ(engFills, refFills) << "fill stream mismatch at op " << i;
      EXPECT_EQ(r.resting(), refId != 0) << "resting mismatch at op " << i;

      if (r.resting()) {
        engIds.push_back(r.orderId);
        refIds.push_back(refId);
      }
    } else if (op < 85) {
      const std::uint32_t qty = qtyDist(rng);

      std::vector<RefFill> engFills;
      (void)engine.addOrder({.type = Engine::OrderType::Market, .quantity = qty,
                             .side = buy ? Engine::Side::Buy : Engine::Side::Sell},
                            [&](const Engine::Fill &f) { engFills.push_back({f.price, f.quantity}); });
      std::vector<RefFill> refFills;
      ref.addMarket(qty, buy, refFills);

      EXPECT_EQ(engFills, refFills) << "market fill stream mismatch at op " << i;
    } else {
      if (!refIds.empty()) {
        const std::size_t idx = std::uniform_int_distribution<std::size_t>(0, refIds.size() - 1)(rng);
        const bool e = engine.cancelOrder(engIds[idx]).cancelled;
        const bool r = ref.cancel(refIds[idx]);
        EXPECT_EQ(e, r) << "cancel mismatch at op " << i;

        refIds[idx] = refIds.back();
        refIds.pop_back();
        engIds[idx] = engIds.back();
        engIds.pop_back();
      }
    }

    if (i % 250 == 0) {
      verifyState(engine, ref);
    }
  }

  verifyState(engine, ref);
}

// ---------------------------------------------------------------------------
// Structural invariant fuzz: no reference oracle, but the book must never
// violate its own invariants (FIFO, per-level volume, best-price, no crossing)
// under a random stream of adds / cancels / modifies with participants.
// ---------------------------------------------------------------------------

using FuzzBook = OrderBook<4096, 128, GenerationalId<4096, 24>, std::uint32_t>;
using FuzzEngine = MatchingEngine<FuzzBook>;

void verifyInvariants(const FuzzEngine &e) {
  const auto &book = e.book();
  std::size_t counted = 0;

  for (std::uint32_t p = 0; p < FuzzBook::kPriceLevels; ++p) {
    const auto price = static_cast<FuzzEngine::Price>(book.base() + p);
    for (const auto side : {FuzzEngine::Side::Buy, FuzzEngine::Side::Sell}) {
      Volume sum = 0;
      auto id = book.head(price, side);
      while (id != FuzzEngine::INVALID_ORDER) {
        ASSERT_TRUE(book.isLive(id)) << "level " << p;
        EXPECT_GT(book.qty(id), 0u);
        EXPECT_EQ(book.priceOf(id), price);
        EXPECT_EQ(book.sideOf(id), side);
        sum += book.qty(id);
        ++counted;
        id = book.nextOf(id);
      }
      EXPECT_EQ(book.volume(price, side), sum) << "level " << p << " side " << static_cast<int>(side);
    }
  }
  EXPECT_EQ(counted, e.openOrders());

  const bool hasBid = e.bestBid() != FuzzEngine::INVALID_PRICE;
  const bool hasAsk = e.bestAsk() != FuzzEngine::INVALID_PRICE;
  if (hasBid && hasAsk) {
    EXPECT_LT(e.bestBid(), e.bestAsk());
  }
}

TEST(OrderBookProperty, InvariantsHoldUnderRandomOps) {
  FuzzEngine e;
  std::vector<FuzzEngine::OrderIdx> live;

  std::mt19937_64 rng(0xC0FFEE);
  std::uniform_int_distribution<std::uint32_t> priceDist(0, FuzzBook::kPriceLevels - 1);
  std::uniform_int_distribution<std::uint32_t> qtyDist(1, 50);
  std::bernoulli_distribution sideDist(0.5);
  std::uniform_int_distribution<int> opDist(0, 99);
  std::uniform_int_distribution<int> tifDist(0, 99);
  std::uniform_int_distribution<std::uint32_t> partDist(1, 4);

  constexpr int kOps = 30000;
  for (int i = 0; i < kOps; ++i) {
    const auto side = sideDist(rng) ? FuzzEngine::Side::Buy : FuzzEngine::Side::Sell;
    const std::uint32_t participant = partDist(rng);
    const int op = opDist(rng);

    if (op < 55) {
      FuzzEngine::Order order;
      order.type = FuzzEngine::OrderType::Limit;
      order.price = static_cast<FuzzEngine::Price>(priceDist(rng));
      order.quantity = qtyDist(rng);
      order.side = side;
      order.participant = participant;
      const int t = tifDist(rng);
      if (t < 60) {
        order.tif = FuzzEngine::TimeInForce::GTC;
      } else if (t < 75) {
        order.tif = FuzzEngine::TimeInForce::IOC;
      } else if (t < 88) {
        order.tif = FuzzEngine::TimeInForce::FOK;
      } else if (t < 93) {
        order.tif = FuzzEngine::TimeInForce::AON;
      } else {
        order.tif = FuzzEngine::TimeInForce::GTC;
        order.postOnly = true;
      }
      const auto r = e.addOrder(order);
      if (r.resting()) {
        live.push_back(r.orderId);
      }
    } else if (op < 72) {
      FuzzEngine::Order order;
      order.type = FuzzEngine::OrderType::Market;
      order.quantity = qtyDist(rng);
      order.side = side;
      order.participant = participant;
      (void)e.addOrder(order);
    } else if (op < 82) {
      if (!live.empty()) {
        const std::size_t idx = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
        (void)e.cancelOrder(live[idx]);
        live[idx] = live.back();
        live.pop_back();
      }
    } else if (op < 90) {
      if (!live.empty()) {
        const std::size_t idx = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
        if (!e.book().isLive(live[idx])) {
          live[idx] = live.back();
          live.pop_back();
        } else {
          FuzzEngine::Order replacement;
          replacement.type = FuzzEngine::OrderType::Limit;
          replacement.tif = FuzzEngine::TimeInForce::GTC;
          replacement.price = static_cast<FuzzEngine::Price>(priceDist(rng));
          replacement.quantity = qtyDist(rng);
          replacement.side = e.book().sideOf(live[idx]);
          replacement.participant = e.book().participantOf(live[idx]);
          const auto r = e.modify(live[idx], replacement);
          live[idx] = live.back();
          live.pop_back();
          if (r.resting()) {
            live.push_back(r.orderId);
          }
        }
      }
    } else if (op < 95) {
      // rebase: shift the price window by a random amount.
      const auto shift = static_cast<FuzzEngine::Price>(static_cast<int>(priceDist(rng)) -
                                                        static_cast<int>(FuzzBook::kPriceLevels) / 2);
      (void)e.rebase(e.base() + shift);
    } else {
      // reduce: execute a live order by a random quantity.
      if (!live.empty()) {
        const std::size_t idx = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
        const auto id = live[idx];
        if (e.book().isLive(id)) {
          const auto remaining = e.book().qty(id);
          if (remaining > 1) {
            const auto qty = static_cast<flatbook::Quantity>(qtyDist(rng)) % (remaining - 1) + 1u;
            (void)e.reduce(id, qty);
          }
        }
      }
    }

    if (i % 200 == 0) {
      verifyInvariants(e);
    }
  }
  verifyInvariants(e);
}

} // namespace
} // namespace flatbook
