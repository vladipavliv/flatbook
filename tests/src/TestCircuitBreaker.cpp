#include <gtest/gtest.h>

#include <vector>

#include "flatbook/CircuitBreaker.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<1024, 256>;
using Engine = MatchingEngine<Book>;
using Guard = CircuitBreaker<Engine>;

Engine::Order limit(long long price, unsigned qty, bool buy) {
  Engine::Order o{};
  o.type = Engine::OrderType::Limit;
  o.tif = Engine::TimeInForce::GTC;
  o.price = price;
  o.quantity = qty;
  o.side = buy ? Engine::Side::Buy : Engine::Side::Sell;
  return o;
}

TEST(CircuitBreaker, RejectsOutsideBand) {
  Engine e;
  Guard g(e, /*reference=*/100, /*band=*/10, /*maxMove=*/50);
  const auto noFill = [](const Engine::Fill &) {};

  EXPECT_TRUE(g.inBand(100));
  EXPECT_TRUE(g.inBand(90));
  EXPECT_TRUE(g.inBand(110));
  EXPECT_FALSE(g.inBand(111));
  EXPECT_FALSE(g.inBand(89));

  auto in = g.addOrder(limit(105, 1, true), noFill);
  EXPECT_EQ(in.result, Engine::Result::Resting);

  auto out = g.addOrder(limit(120, 1, true), noFill);
  EXPECT_EQ(out.result, Engine::Result::Rejected);
  EXPECT_EQ(out.reason, Engine::Reason::PriceOutOfWindow);
  EXPECT_EQ(e.openOrders(), 1u); // the out-of-band order never reached the book
}

TEST(CircuitBreaker, VolatilityMoveHalts) {
  Engine e;
  Guard g(e, /*reference=*/100, /*band=*/50, /*maxMove=*/5);
  const auto noFill = [](const Engine::Fill &) {};

  // Rest a sell at 110 and trade at 110: the fill moves the reference by 10,
  // which exceeds maxMove 5 and halts the market.
  (void)g.addOrder(limit(110, 1, false), noFill);
  EXPECT_FALSE(g.halted());

  (void)g.addOrder(limit(110, 1, true), noFill);
  EXPECT_TRUE(g.halted());
  EXPECT_EQ(g.reference(), 110);

  // While halted, every order is rejected.
  auto r = g.addOrder(limit(100, 1, true), noFill);
  EXPECT_EQ(r.result, Engine::Result::Rejected);
}

TEST(CircuitBreaker, ManualHaltAndResume) {
  Engine e;
  Guard g(e, /*reference=*/100, /*band=*/10, /*maxMove=*/50);
  const auto noFill = [](const Engine::Fill &) {};

  g.halt();
  EXPECT_TRUE(g.halted());
  EXPECT_EQ(g.addOrder(limit(100, 1, true), noFill).result, Engine::Result::Rejected);

  g.resume(/*newReference=*/200);
  EXPECT_FALSE(g.halted());
  EXPECT_EQ(g.reference(), 200);

  // The band now centres on 200: 190..210 pass, 180 does not.
  EXPECT_EQ(g.addOrder(limit(200, 1, true), noFill).result, Engine::Result::Resting);
  EXPECT_EQ(g.addOrder(limit(180, 1, true), noFill).result, Engine::Result::Rejected);
}

TEST(CircuitBreaker, ReferenceTracksTrades) {
  Engine e;
  Guard g(e, /*reference=*/100, /*band=*/100, /*maxMove=*/100);
  const auto noFill = [](const Engine::Fill &) {};

  (void)g.addOrder(limit(105, 3, false), noFill); // rest a sell at 105

  // A marketable buy fills at 105; the reference moves to 105.
  (void)g.addOrder(limit(105, 2, true), noFill);
  EXPECT_EQ(g.reference(), 105);
}

TEST(CircuitBreaker, SweepHaltsWithoutALargeSingleStep) {
  Engine e;
  Guard g(e, /*reference=*/100, /*band=*/1000, /*maxMove=*/5);
  const auto noFill = [](const Engine::Fill &) {};

  // Rest sells at 102, 106, 110, 114: a sweep walks the book in steps of 4,
  // each below maxMove 5, but the total move from the order-start reference
  // (100) must still trigger the halt.
  (void)g.addOrder(limit(102, 1, false), noFill);
  (void)g.addOrder(limit(106, 1, false), noFill);
  (void)g.addOrder(limit(110, 1, false), noFill);
  (void)g.addOrder(limit(114, 1, false), noFill);

  (void)g.addOrder(limit(114, 4, true), noFill);
  EXPECT_TRUE(g.halted());
  EXPECT_EQ(g.reference(), 114);
}

TEST(CircuitBreaker, ModifyIsGuarded) {
  Engine e;
  Guard g(e, /*reference=*/100, /*band=*/10, /*maxMove=*/50);
  const auto noFill = [](const Engine::Fill &) {};

  auto r = g.addOrder(limit(100, 1, true), noFill);
  ASSERT_EQ(r.result, Engine::Result::Resting);

  // Modifying to a price outside the band is rejected before reaching the book.
  auto m = g.modify(r.orderId, limit(120, 1, true), noFill);
  EXPECT_EQ(m.result, Engine::Result::Rejected);
  EXPECT_EQ(m.reason, Engine::Reason::PriceOutOfWindow);
  EXPECT_EQ(e.bestBid(), 100); // original order untouched

  // In-band modify still works.
  m = g.modify(r.orderId, limit(105, 1, true), noFill);
  EXPECT_EQ(m.result, Engine::Result::Resting);
  EXPECT_EQ(e.bestBid(), 105);
}

} // namespace
} // namespace flatbook
