#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "flatbook/Journal.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<4096, 128, GenerationalId<4096, 24>, std::uint32_t>;
using Engine = MatchingEngine<Book>;
using Order = Engine::Order;
using OrderIdx = Engine::OrderIdx;
using Price = Engine::Price;
using Side = Engine::Side;

Order makeOrder(std::mt19937_64 &rng, Price base) {
  Order o;
  const int t = static_cast<int>(rng() % 100);
  o.type = (t < 12) ? Engine::OrderType::Market : Engine::OrderType::Limit;
  o.tif = static_cast<Engine::TimeInForce>(rng() % 3);
  o.postOnly = (t >= 95);
  o.price = base + static_cast<Price>(rng() % Book::kPriceLevels);
  o.quantity = static_cast<Quantity>(1 + rng() % 50);
  o.side = (rng() & 1) ? Side::Buy : Side::Sell;
  o.participant = static_cast<std::uint32_t>(1 + rng() % 4);
  return o;
}

// Verify two engines hold identical state: window, sequence, open orders, best
// prices, and every level's volume and FIFO contents (ids include generation).
void expectSameState(const Engine &a, const Engine &b) {
  ASSERT_EQ(a.base(), b.base());
  ASSERT_EQ(a.openOrders(), b.openOrders());
  ASSERT_EQ(a.lastSequence(), b.lastSequence());
  ASSERT_EQ(a.bestBid(), b.bestBid());
  ASSERT_EQ(a.bestAsk(), b.bestAsk());

  const Price base = a.base();
  for (std::uint32_t s = 0; s < Book::kPriceLevels; ++s) {
    const Price price = base + static_cast<Price>(s);
    for (const Side side : {Side::Buy, Side::Sell}) {
      EXPECT_EQ(a.book().volume(price, side), b.book().volume(price, side))
          << "slot " << s << " side " << static_cast<int>(side);

      auto ida = a.book().head(price, side);
      auto idb = b.book().head(price, side);
      while (ida != Engine::INVALID_ORDER && idb != Engine::INVALID_ORDER) {
        EXPECT_EQ(ida, idb); // index AND generation must match
        EXPECT_EQ(a.book().qty(ida), b.book().qty(idb));
        EXPECT_EQ(a.book().priceOf(ida), b.book().priceOf(idb));
        ida = a.book().nextOf(ida);
        idb = b.book().nextOf(idb);
      }
      EXPECT_EQ(ida == Engine::INVALID_ORDER, idb == Engine::INVALID_ORDER);
    }
  }
}

// Drive `ops` random operations through `engine`, recording each to `journal`.
void runOps(Engine &engine, Journal<Engine> &journal, std::vector<OrderIdx> &live,
            std::mt19937_64 &rng, Price &base, int ops) {
  for (int i = 0; i < ops; ++i) {
    const int r = static_cast<int>(rng() % 100);
    std::vector<Engine::Fill> fills;

    if (r < 8 && !live.empty()) {
      const std::size_t idx = static_cast<std::size_t>(rng() % live.size());
      const auto id = live[idx];
      const auto res = engine.cancelOrder(id);
      journal.recordCancel(id, res);
      live[idx] = live.back();
      live.pop_back();
    } else if (r < 12) {
      const Price newBase = base + static_cast<Price>(rng() % 40) - 20;
      const auto res = engine.rebase(newBase);
      journal.recordRebase(newBase, res);
      base = newBase;
      // prune ids that fell out of the window (now stale)
      std::vector<OrderIdx> pruned;
      pruned.reserve(live.size());
      for (const auto id : live) {
        if (engine.book().isLive(id)) {
          pruned.push_back(id);
        }
      }
      live.swap(pruned);
    } else if (r < 20 && !live.empty()) {
      const std::size_t idx = static_cast<std::size_t>(rng() % live.size());
      const auto id = live[idx];
      Order repl = makeOrder(rng, base);
      repl.type = Engine::OrderType::Limit;
      repl.tif = Engine::TimeInForce::GTC;
      repl.side = engine.book().sideOf(id);
      const auto res = engine.modify(id, repl, [&](const Engine::Fill &f) { fills.push_back(f); });
      journal.recordModify(id, repl, res, fills);
      live[idx] = live.back();
      live.pop_back();
      if (res.resting()) {
        live.push_back(res.orderId);
      }
    } else {
      const Order order = makeOrder(rng, base);
      const auto res = engine.addOrder(order, [&](const Engine::Fill &f) { fills.push_back(f); });
      journal.recordAdd(order, res, fills);
      if (res.resting()) {
        live.push_back(res.orderId);
      }
    }
  }
}

TEST(Journal, ReplayReproducesStateAndResults) {
  Engine a;
  Journal<Engine> journal;
  std::vector<OrderIdx> live;
  std::mt19937_64 rng(0x5EEDC0DE);
  Price base = 0;

  runOps(a, journal, live, rng, base, 20000);

  Engine replay;
  EXPECT_EQ(replayInto(journal, replay), 0u);
  expectSameState(a, replay);
}

TEST(Journal, SnapshotThenReplayTailReproducesState) {
  Engine a;
  Journal<Engine> journal;
  std::vector<OrderIdx> live;
  std::mt19937_64 rng(0x0A11CE);
  Price base = 0;

  runOps(a, journal, live, rng, base, 5000);

  // Checkpoint: snapshot A and remember the journal position.
  const auto snap = a.snapshot();
  const std::size_t snapIndex = journal.size();

  runOps(a, journal, live, rng, base, 15000);

  // Restore B from the snapshot and replay only the tail.
  Engine b;
  b.restore(snap);
  EXPECT_EQ(replayInto(journal, b, snapIndex), 0u);
  expectSameState(a, b);
}

TEST(Journal, EmptyJournalReplaysToEmptyEngine) {
  Engine a;
  Journal<Engine> journal;
  Engine replay;
  EXPECT_EQ(replayInto(journal, replay), 0u);
  expectSameState(a, replay);
}

} // namespace
} // namespace flatbook
