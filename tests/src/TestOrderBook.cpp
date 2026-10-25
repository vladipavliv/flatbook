#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "flatbook/MatchingEngine.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<1024, 256>;
using Engine = MatchingEngine<Book>;
using Side = Engine::Side;
using OrderType = Engine::OrderType;
using TimeInForce = Engine::TimeInForce;
using Result = Engine::Result;
using Fills = std::vector<Engine::Fill>;

template <typename EngineT>
auto submit(EngineT &engine, const typename EngineT::Order &order,
            std::vector<typename EngineT::Fill> &fills) {
  return engine.addOrder(order, [&](const typename EngineT::Fill &f) { fills.push_back(f); });
}

Engine::Order limit(Engine::Price price, Quantity qty, Side side) {
  return {.type = OrderType::Limit,
          .tif = TimeInForce::GTC,
          .price = price,
          .quantity = qty,
          .side = side};
}

// ---------------------------------------------------------------------------
// Limit GTC: resting and crossing
// ---------------------------------------------------------------------------

TEST(OrderBook, LimitRestsWhenNonCrossing) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  auto r = submit(e, limit(99, 5, Side::Buy), fills);

  EXPECT_EQ(r.result, Result::Resting);
  EXPECT_TRUE(r.resting());
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(e.openOrders(), 2u);
  EXPECT_EQ(e.bestBid(), 99u);
  EXPECT_EQ(e.bestAsk(), 100u);
}

TEST(OrderBook, LimitCrossesAtEqualPrice) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  auto r = submit(e, limit(100, 10, Side::Buy), fills);

  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 10u);
  EXPECT_EQ(fills[0].price, 100u);
  EXPECT_EQ(fills[0].side, Side::Sell);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, PartialFillRestsRemainder) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  auto r = submit(e, limit(100, 15, Side::Buy), fills);

  EXPECT_EQ(r.result, Result::Resting);
  EXPECT_EQ(e.book().volume(100, Side::Buy), 5u);
  EXPECT_EQ(e.openOrders(), 1u);
}

TEST(OrderBook, PriceTimePriorityIsFifoWithinLevel) {
  Engine e;
  Fills fills;

  auto first = submit(e, limit(100, 5, Side::Sell), fills);
  auto second = submit(e, limit(100, 7, Side::Sell), fills);
  submit(e, limit(100, 8, Side::Buy), fills);

  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].orderId, first.orderId);
  EXPECT_EQ(fills[0].quantity, 5u);
  EXPECT_EQ(fills[1].orderId, second.orderId);
  EXPECT_EQ(fills[1].quantity, 3u);
}

TEST(OrderBook, BestPriceHasPriorityOverLaterPrice) {
  Engine e;
  Fills fills;

  submit(e, limit(101, 5, Side::Sell), fills);         // worse ask
  submit(e, limit(100, 7, Side::Sell), fills);         // better ask
  auto r = submit(e, limit(101, 8, Side::Buy), fills); // price high enough to reach both

  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].price, 100u);
  EXPECT_EQ(fills[0].quantity, 7u);
  EXPECT_EQ(fills[1].price, 101u);
  EXPECT_EQ(fills[1].quantity, 1u);
}

TEST(OrderBook, SellSideCrossesBid) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Buy), fills);
  auto r = submit(e, limit(99, 5, Side::Sell), fills);

  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].price, 100u);
  EXPECT_EQ(fills[0].side, Side::Buy);
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TEST(OrderBook, MarketFillsAcrossLevelsThenStops) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  submit(e, limit(101, 7, Side::Sell), fills);
  auto r = submit(e, {.type = OrderType::Market, .quantity = 20, .side = Side::Buy}, fills);

  EXPECT_EQ(r.result, Result::Cancelled);
  EXPECT_EQ(r.remaining, 3u);
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].price, 100u);
  EXPECT_EQ(fills[1].price, 101u);
  EXPECT_EQ(e.openOrders(), 0u);
}

// ---------------------------------------------------------------------------
// IOC
// ---------------------------------------------------------------------------

TEST(OrderBook, IocCancelsUnfilledRemainder) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::IOC,
                   .price = 100,
                   .quantity = 15,
                   .side = Side::Buy},
                  fills);

  EXPECT_EQ(r.result, Result::Cancelled);
  EXPECT_EQ(r.remaining, 5u);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 10u);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, IocWithNoMatchCancelsFully) {
  Engine e;
  Fills fills;

  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::IOC,
                   .price = 100,
                   .quantity = 5,
                   .side = Side::Buy},
                  fills);

  EXPECT_EQ(r.result, Result::Cancelled);
  EXPECT_EQ(r.remaining, 5u);
  EXPECT_TRUE(fills.empty());
}

// ---------------------------------------------------------------------------
// FOK
// ---------------------------------------------------------------------------

TEST(OrderBook, FokFillsFullyWhenLiquidityIsEnough) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::FOK,
                   .price = 100,
                   .quantity = 10,
                   .side = Side::Buy},
                  fills);

  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 10u);
}

TEST(OrderBook, FokRejectsWithoutTouchingBook) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 5, Side::Sell), fills);
  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::FOK,
                   .price = 100,
                   .quantity = 10,
                   .side = Side::Buy},
                  fills);

  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(e.book().volume(100, Side::Sell), 5u); // untouched
  EXPECT_EQ(e.openOrders(), 1u);
}

TEST(OrderBook, MarketFokIsAllOrNothing) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);

  auto rejected = submit(
      e, {.type = OrderType::Market, .tif = TimeInForce::FOK, .quantity = 11, .side = Side::Buy},
      fills);
  EXPECT_EQ(rejected.result, Result::Rejected);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(e.book().volume(100, Side::Sell), 10u);

  auto filled = submit(
      e, {.type = OrderType::Market, .tif = TimeInForce::FOK, .quantity = 10, .side = Side::Buy},
      fills);
  EXPECT_EQ(filled.result, Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
}

// ---------------------------------------------------------------------------
// Post-only
// ---------------------------------------------------------------------------

TEST(OrderBook, PostOnlyRestsWhenNonCrossing) {
  Engine e;
  Fills fills;

  submit(e, limit(101, 10, Side::Sell), fills);
  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::GTC,
                   .postOnly = true,
                   .price = 100,
                   .quantity = 5,
                   .side = Side::Buy},
                  fills);

  EXPECT_EQ(r.result, Result::Resting);
  EXPECT_EQ(e.bestBid(), 100u);
  EXPECT_TRUE(fills.empty());
}

TEST(OrderBook, PostOnlyRejectsWhenWouldCross) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::GTC,
                   .postOnly = true,
                   .price = 100,
                   .quantity = 5,
                   .side = Side::Buy},
                  fills);

  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(e.book().volume(100, Side::Sell), 10u); // untouched
  EXPECT_EQ(e.openOrders(), 1u);
}

TEST(OrderBook, PostOnlyInvalidWithMarketOrNonGtc) {
  Engine e;
  Fills fills;

  auto market = submit(
      e, {.type = OrderType::Market, .postOnly = true, .quantity = 5, .side = Side::Buy}, fills);
  EXPECT_EQ(market.result, Result::Rejected);

  auto ioc = submit(e,
                    {.type = OrderType::Limit,
                     .tif = TimeInForce::IOC,
                     .postOnly = true,
                     .price = 100,
                     .quantity = 5,
                     .side = Side::Buy},
                    fills);
  EXPECT_EQ(ioc.result, Result::Rejected);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST(OrderBook, CancelRemovesRestingOrder) {
  Engine e;
  Fills fills;

  auto r = submit(e, limit(100, 5, Side::Sell), fills);
  EXPECT_TRUE(e.cancelOrder(r.orderId).cancelled);
  EXPECT_EQ(e.openOrders(), 0u);
  EXPECT_EQ(e.bestAsk(), Engine::INVALID_PRICE);

  EXPECT_FALSE(e.cancelOrder(r.orderId).cancelled); // double cancel
}

// ---------------------------------------------------------------------------
// Capacity / recycling
// ---------------------------------------------------------------------------

TEST(OrderBook, CapacityExhaustionRejectsNewOrder) {
  using SmallBook = OrderBook<8, 32>;
  using SmallEngine = MatchingEngine<SmallBook>;
  SmallEngine e;
  std::vector<SmallEngine::Fill> fills;

  const SmallEngine::Order sell{.type = SmallEngine::OrderType::Limit,
                                .tif = SmallEngine::TimeInForce::GTC,
                                .price = 10,
                                .quantity = 1,
                                .side = SmallEngine::Side::Sell};

  for (int i = 0; i < 7; ++i) { // Size - 1: id 0 is the reserved null sentinel
    auto r = submit(e, sell, fills);
    EXPECT_TRUE(r.resting()) << "order " << i << " should rest";
  }

  EXPECT_EQ(e.openOrders(), 7u);

  auto full = submit(e, sell, fills);
  EXPECT_EQ(full.result, SmallEngine::Result::Rejected);
  EXPECT_EQ(full.remaining, 1u);
  EXPECT_EQ(e.openOrders(), 7u);
}

TEST(OrderBook, ReleasedIdsAreReusedLifo) {
  Engine e;
  Fills fills;

  auto first = submit(e, limit(100, 5, Side::Sell), fills);
  EXPECT_TRUE(e.cancelOrder(first.orderId).cancelled);

  auto second = submit(e, limit(101, 5, Side::Sell), fills);
  EXPECT_EQ(second.orderId, first.orderId);
}

// ---------------------------------------------------------------------------
// Lifecycle / input validation
// ---------------------------------------------------------------------------

TEST(OrderBook, ClearResetsState) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  submit(e, limit(99, 10, Side::Buy), fills);

  e.clear();

  EXPECT_EQ(e.openOrders(), 0u);
  EXPECT_EQ(e.bestBid(), Engine::INVALID_PRICE);
  EXPECT_EQ(e.bestAsk(), Engine::INVALID_PRICE);
}

TEST(OrderBook, ZeroQuantityRejected) {
  Engine e;
  Fills fills;

  auto r = submit(e, limit(100, 0, Side::Buy), fills);
  EXPECT_EQ(r.result, Result::Rejected);
}

TEST(OrderBook, PriceOutOfRangeRejected) {
  Engine e;
  Fills fills;

  auto r = submit(e, limit(Book::kPriceLevels, 5, Side::Buy), fills);
  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_EQ(r.reason, Engine::Reason::Invalid);
}

TEST(OrderBook, RejectReasonsAreDistinct) {
  Engine e;
  Fills fills;

  // Post-only that would cross -> PostOnly.
  submit(e, limit(100, 5, Side::Sell), fills);
  auto postOnly = submit(e,
                         {.type = OrderType::Limit,
                          .tif = TimeInForce::GTC,
                          .postOnly = true,
                          .price = 100,
                          .quantity = 5,
                          .side = Side::Buy},
                         fills);
  EXPECT_EQ(postOnly.result, Result::Rejected);
  EXPECT_EQ(postOnly.reason, Engine::Reason::PostOnly);

  // FOK that cannot fill -> Fok.
  auto fok = submit(e,
                    {.type = OrderType::Limit,
                     .tif = TimeInForce::FOK,
                     .price = 100,
                     .quantity = 10,
                     .side = Side::Buy},
                    fills);
  EXPECT_EQ(fok.result, Result::Rejected);
  EXPECT_EQ(fok.reason, Engine::Reason::Fok);

  // A normal filled order has no reason.
  auto filled = submit(e, limit(100, 5, Side::Buy), fills);
  EXPECT_EQ(filled.result, Result::Filled);
  EXPECT_EQ(filled.reason, Engine::Reason::None);
}

TEST(OrderBook, SequenceIsMonotonic) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  auto b = submit(e, limit(99, 5, Side::Buy), fills);

  EXPECT_EQ(a.seq, 1u);
  EXPECT_EQ(b.seq, 2u);
  EXPECT_EQ(e.lastSequence(), 2u);
}

TEST(OrderBook, ModifyReplacesOrder) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  ASSERT_TRUE(a.resting());

  auto r = e.modify(a.orderId, limit(101, 7, Side::Sell),
                    [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Resting);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.book().volume(100, Side::Sell), 0u);
  EXPECT_EQ(e.book().volume(101, Side::Sell), 7u);
}

TEST(OrderBook, ModifyRejectsStaleId) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  ASSERT_TRUE(a.resting());
  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled);

  auto r = e.modify(a.orderId, limit(101, 5, Side::Sell),
                    [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, TopPriceLevelBoundary) {
  Engine e;
  Fills fills;

  // Rest at the top price level, then drain it past the boundary.
  submit(e, limit(Book::kPriceLevels - 1, 5, Side::Sell), fills);

  auto m = submit(e, {.type = OrderType::Market, .quantity = 10, .side = Side::Buy}, fills);
  EXPECT_EQ(m.result, Result::Cancelled);
  EXPECT_EQ(m.remaining, 5u);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, FokAtTopPriceLevelRejected) {
  Engine e;
  Fills fills;

  submit(e, limit(Book::kPriceLevels - 1, 5, Side::Sell), fills);

  auto r = submit(e,
                  {.type = OrderType::Limit,
                   .tif = TimeInForce::FOK,
                   .price = Book::kPriceLevels - 1,
                   .quantity = 10,
                   .side = Side::Buy},
                  fills);
  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_EQ(e.openOrders(), 1u); // sell untouched
}

// ---------------------------------------------------------------------------
// Price window / rebase
// ---------------------------------------------------------------------------

TEST(OrderBook, RebaseKeepsOrdersAndMatching) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  ASSERT_TRUE(a.resting());

  const auto cancelled = e.rebase(50).cancelled;
  EXPECT_TRUE(cancelled.empty());
  EXPECT_EQ(e.base(), 50);
  EXPECT_EQ(e.bestAsk(), 100);
  EXPECT_EQ(e.book().priceOf(a.orderId), 100); // logical price unchanged

  auto r = submit(e, {.type = OrderType::Market, .quantity = 5, .side = Side::Buy}, fills);
  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].price, 100);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, RebaseCancelsOutOfWindowOrders) {
  Engine e;
  Fills fills;

  auto low = submit(e, limit(5, 1, Side::Buy), fills);
  auto high = submit(e, limit(250, 1, Side::Sell), fills);
  ASSERT_TRUE(low.resting());
  ASSERT_TRUE(high.resting());

  const auto cancelled = e.rebase(100).cancelled;
  ASSERT_EQ(cancelled.size(), 1u);
  EXPECT_EQ(cancelled[0], low.orderId);
  EXPECT_FALSE(e.book().isLive(low.orderId));
  EXPECT_TRUE(e.book().isLive(high.orderId));
  EXPECT_EQ(e.bestBid(), Engine::INVALID_PRICE);
  EXPECT_EQ(e.bestAsk(), 250);
}

TEST(OrderBook, ModifyInvalidReplacementKeepsOrder) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  ASSERT_TRUE(a.resting());

  // Invalid replacement (price out of range) must leave the old order untouched.
  auto r = e.modify(a.orderId, limit(Book::kPriceLevels, 5, Side::Sell),
                    [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.book().volume(100, Side::Sell), 5u);
  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled);
}

TEST(OrderBook, ModifySideChangeRejected) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  ASSERT_TRUE(a.resting());

  // Side-changing replace must be rejected, leaving the order untouched.
  auto r = e.modify(a.orderId, limit(100, 5, Side::Buy),
                    [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.book().volume(100, Side::Sell), 5u);
  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled);
}

TEST(OrderBook, ModifyNonGtcRejected) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  ASSERT_TRUE(a.resting());

  auto r = e.modify(a.orderId,
                    {.type = OrderType::Limit,
                     .tif = TimeInForce::IOC,
                     .price = 100,
                     .quantity = 5,
                     .side = Side::Sell},
                    [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Rejected);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled);
}

// ---------------------------------------------------------------------------
// Generational ids (ABA protection)
// ---------------------------------------------------------------------------

TEST(OrderBook, GenerationalIdRejectsDoubleCancel) {
  using GenBook = OrderBook<64, 256, GenerationalId<64, 16>>;
  using GenEngine = MatchingEngine<GenBook>;
  GenEngine e;
  std::vector<GenEngine::Fill> fills;

  const GenEngine::Order sell{.type = GenEngine::OrderType::Limit,
                              .tif = GenEngine::TimeInForce::GTC,
                              .price = 100,
                              .quantity = 5,
                              .side = GenEngine::Side::Sell};

  auto a = e.addOrder(sell, [&](const GenEngine::Fill &f) { fills.push_back(f); });
  ASSERT_TRUE(a.resting());

  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled);
  EXPECT_FALSE(e.cancelOrder(a.orderId).cancelled); // stale: slot is empty again
}

TEST(OrderBook, GenerationalIdRejectsStaleCancelAfterReuse) {
  using GenBook = OrderBook<64, 256, GenerationalId<64, 16>>;
  using GenEngine = MatchingEngine<GenBook>;
  GenEngine e;
  std::vector<GenEngine::Fill> fills;

  const auto makeSell = [](GenEngine::Price price) {
    return GenEngine::Order{.type = GenEngine::OrderType::Limit,
                            .tif = GenEngine::TimeInForce::GTC,
                            .price = price,
                            .quantity = 5,
                            .side = GenEngine::Side::Sell};
  };

  auto a = e.addOrder(makeSell(100), [&](const GenEngine::Fill &f) { fills.push_back(f); });
  ASSERT_TRUE(a.resting());

  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled); // slot returned to the pool

  auto b = e.addOrder(makeSell(101), [&](const GenEngine::Fill &f) { fills.push_back(f); });
  ASSERT_TRUE(b.resting());

  // Same slot index, but generation was bumped on reuse.
  EXPECT_EQ(a.orderId.index(), b.orderId.index());
  EXPECT_NE(a.orderId.generation(), b.orderId.generation());

  // Stale id must not cancel the new order...
  EXPECT_FALSE(e.cancelOrder(a.orderId).cancelled);
  EXPECT_EQ(e.openOrders(), 1u);

  // ...while the fresh id does.
  EXPECT_TRUE(e.cancelOrder(b.orderId).cancelled);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, GenerationalIdSurvivesCloseAndEmptyWindow) {
  using GenBook = OrderBook<64, 256, GenerationalId<64, 16>>;
  using GenEngine = MatchingEngine<GenBook>;
  GenEngine e;
  std::vector<GenEngine::Fill> fills;

  const GenEngine::Order sell{.type = GenEngine::OrderType::Limit,
                              .tif = GenEngine::TimeInForce::GTC,
                              .price = 100,
                              .quantity = 5,
                              .side = GenEngine::Side::Sell};

  auto a = e.addOrder(sell, [&](const GenEngine::Fill &f) { fills.push_back(f); });
  EXPECT_TRUE(e.cancelOrder(a.orderId).cancelled);

  // Slot is now empty and freed; a stale cancel must not resurrect anything.
  EXPECT_FALSE(e.cancelOrder(a.orderId).cancelled);
  EXPECT_EQ(e.openOrders(), 0u);
}

// ---------------------------------------------------------------------------
// Self-trade prevention (participant key)
// ---------------------------------------------------------------------------

using StpBook = OrderBook<1024, 256, RawId<1024>, std::uint32_t>;
using StpEngine = MatchingEngine<StpBook>;

TEST(OrderBook, SelfTradePreventionSkipsOwnOrders) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 1},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });
  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 3,
                    .side = StpEngine::Side::Sell,
                    .participant = 1},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });

  auto r = e.addOrder({.type = StpEngine::OrderType::Limit,
                       .tif = StpEngine::TimeInForce::GTC,
                       .price = 100,
                       .quantity = 8,
                       .side = StpEngine::Side::Buy,
                       .participant = 1},
                      [&](const StpEngine::Fill &f) { fills.push_back(f); });

  // cancel-newest: the buy's remainder would cross its own sells, so it is
  // cancelled instead of rested - no crossed book.
  EXPECT_EQ(r.result, StpEngine::Result::Cancelled);
  EXPECT_EQ(r.remaining, 8u);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(e.openOrders(), 2u);
  EXPECT_EQ(e.book().volume(100, StpEngine::Side::Sell), 8u);
  EXPECT_EQ(e.book().volume(100, StpEngine::Side::Buy), 0u);
}

TEST(OrderBook, SelfTradePreventionMatchesOtherParticipant) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 1},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });

  auto r = e.addOrder({.type = StpEngine::OrderType::Limit,
                       .tif = StpEngine::TimeInForce::GTC,
                       .price = 100,
                       .quantity = 5,
                       .side = StpEngine::Side::Buy,
                       .participant = 2},
                      [&](const StpEngine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, StpEngine::Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, SelfTradePreventionSkipsSelfThenMatchesOther) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 1},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });
  auto b = e.addOrder({.type = StpEngine::OrderType::Limit,
                       .tif = StpEngine::TimeInForce::GTC,
                       .price = 100,
                       .quantity = 5,
                       .side = StpEngine::Side::Sell,
                       .participant = 2},
                      [&](const StpEngine::Fill &f) { fills.push_back(f); });

  auto r = e.addOrder({.type = StpEngine::OrderType::Limit,
                       .tif = StpEngine::TimeInForce::GTC,
                       .price = 100,
                       .quantity = 5,
                       .side = StpEngine::Side::Buy,
                       .participant = 1},
                      [&](const StpEngine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, StpEngine::Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].orderId, b.orderId); // matched B, not A's own order
  EXPECT_EQ(e.openOrders(), 1u);          // only A's sell remains
  EXPECT_EQ(e.book().volume(100, StpEngine::Side::Sell), 5u);
}

// ---------------------------------------------------------------------------
// Fill reporting details
// ---------------------------------------------------------------------------

TEST(OrderBook, FillCarriesSequenceAndRestingSide) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  const auto aggressor = submit(e, limit(100, 6, Side::Buy), fills);

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].seq, aggressor.seq);
  EXPECT_EQ(fills[0].side, Side::Sell); // the resting order's side
  EXPECT_EQ(fills[0].price, 100u);
}

TEST(OrderBook, FillOrderIdIsRestingOrder) {
  Engine e;
  Fills fills;

  const auto resting = submit(e, limit(100, 10, Side::Sell), fills);
  submit(e, limit(100, 6, Side::Buy), fills);

  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].orderId, resting.orderId);
}

TEST(OrderBook, MarketExactFillIsFilled) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 10, Side::Sell), fills);
  const auto r = submit(e, {.type = OrderType::Market, .quantity = 10, .side = Side::Buy}, fills);

  EXPECT_EQ(r.result, Result::Filled);
  EXPECT_EQ(r.remaining, 0u);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, MarketWithNoLiquidityIsCancelled) {
  Engine e;
  Fills fills;

  const auto r = submit(e, {.type = OrderType::Market, .quantity = 10, .side = Side::Buy}, fills);

  EXPECT_EQ(r.result, Result::Cancelled);
  EXPECT_EQ(r.remaining, 10u);
  EXPECT_TRUE(fills.empty());
}

TEST(OrderBook, FokExactFillAcrossLevels) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 5, Side::Sell), fills);
  submit(e, limit(101, 5, Side::Sell), fills);

  const auto r = submit(e,
                        {.type = OrderType::Limit,
                         .tif = TimeInForce::FOK,
                         .price = 101,
                         .quantity = 10,
                         .side = Side::Buy},
                        fills);

  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].price, 100u);
  EXPECT_EQ(fills[1].price, 101u);
  EXPECT_EQ(e.openOrders(), 0u);
}

// ---------------------------------------------------------------------------
// Modify semantics
// ---------------------------------------------------------------------------

TEST(OrderBook, ModifyLosesTimePriority) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills);
  auto b = submit(e, limit(100, 7, Side::Sell), fills);
  ASSERT_TRUE(a.resting());
  ASSERT_TRUE(b.resting());

  // Replace A at the same price; it must move to the back of the queue.
  (void)e.modify(a.orderId, limit(100, 3, Side::Sell),
                 [&](const Engine::Fill &f) { fills.push_back(f); });

  submit(e, limit(100, 9, Side::Buy), fills);
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].orderId, b.orderId); // original head B fills first
  EXPECT_EQ(fills[0].quantity, 7u);
  EXPECT_EQ(fills[1].quantity, 2u); // then the modified A (3 left, 2 needed)
  EXPECT_EQ(e.book().volume(100, Side::Sell), 1u);
}

TEST(OrderBook, ModifyConsumesSequenceEvenWhenRejected) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills); // seq 1
  ASSERT_TRUE(a.resting());
  const auto before = e.lastSequence();

  (void)e.modify(Engine::INVALID_ORDER, limit(101, 5, Side::Sell),
                 [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(e.lastSequence(), before + 1); // reject still consumes one sequence
}

TEST(OrderBook, ModifySuccessfulAdvancesSequenceByTwo) {
  Engine e;
  Fills fills;

  auto a = submit(e, limit(100, 5, Side::Sell), fills); // seq 1
  ASSERT_TRUE(a.resting());

  const auto r = e.modify(a.orderId, limit(101, 5, Side::Sell),
                          [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Resting);
  EXPECT_EQ(e.lastSequence(), 3u); // modify + replacement each consume one
  EXPECT_EQ(r.seq, 3u);
}

TEST(OrderBook, ModifyThenCrosses) {
  Engine e;
  Fills fills;

  submit(e, limit(110, 5, Side::Sell), fills);
  auto bid = submit(e, limit(100, 5, Side::Buy), fills);
  ASSERT_TRUE(bid.resting());

  const auto r = e.modify(bid.orderId, limit(110, 5, Side::Buy),
                          [&](const Engine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 5u);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, ClearResetsSequence) {
  Engine e;
  Fills fills;

  submit(e, limit(100, 5, Side::Sell), fills);
  submit(e, limit(99, 5, Side::Buy), fills);
  EXPECT_EQ(e.lastSequence(), 2u);

  e.clear();
  EXPECT_EQ(e.lastSequence(), 0u);

  const auto r = submit(e, limit(100, 5, Side::Sell), fills);
  EXPECT_EQ(r.seq, 1u);
}

// ---------------------------------------------------------------------------
// Self-trade prevention edge cases
// ---------------------------------------------------------------------------

TEST(OrderBook, PostOnlyRejectsWhenOnlyOwnOrdersCross) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 1},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });

  // post-only buy at the same price, same participant: it would cross its own
  // order only, but postOnly does not skip self, so it is rejected.
  const auto r = e.addOrder({.type = StpEngine::OrderType::Limit,
                             .tif = StpEngine::TimeInForce::GTC,
                             .postOnly = true,
                             .price = 100,
                             .quantity = 5,
                             .side = StpEngine::Side::Buy,
                             .participant = 1},
                            [&](const StpEngine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, StpEngine::Result::Rejected);
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(e.openOrders(), 1u);
}

TEST(OrderBook, StpSkipsSelfThenMatchesOtherAndCancelsNewest) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 1},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });
  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 2},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });

  const auto r = e.addOrder({.type = StpEngine::OrderType::Limit,
                             .tif = StpEngine::TimeInForce::GTC,
                             .price = 100,
                             .quantity = 8,
                             .side = StpEngine::Side::Buy,
                             .participant = 1},
                            [&](const StpEngine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, StpEngine::Result::Cancelled);
  EXPECT_EQ(r.remaining, 3u);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].quantity, 5u); // matched participant 2 only
  EXPECT_EQ(e.openOrders(), 1u);    // participant 1's sell remains
  EXPECT_EQ(e.book().volume(100, StpEngine::Side::Sell), 5u);
}

TEST(OrderBook, StpParticipantZeroDisablesStp) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  (void)e.addOrder({.type = StpEngine::OrderType::Limit,
                    .tif = StpEngine::TimeInForce::GTC,
                    .price = 100,
                    .quantity = 5,
                    .side = StpEngine::Side::Sell,
                    .participant = 0},
                   [&](const StpEngine::Fill &f) { fills.push_back(f); });

  // participant 0 means "no STP": the buy crosses the sell despite the same key.
  const auto r = e.addOrder({.type = StpEngine::OrderType::Limit,
                             .tif = StpEngine::TimeInForce::GTC,
                             .price = 100,
                             .quantity = 5,
                             .side = StpEngine::Side::Buy,
                             .participant = 0},
                            [&](const StpEngine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, StpEngine::Result::Filled);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(e.openOrders(), 0u);
}

TEST(OrderBook, ModifyInheritsParticipant) {
  StpEngine e;
  std::vector<StpEngine::Fill> fills;

  const auto a = e.addOrder({.type = StpEngine::OrderType::Limit,
                             .tif = StpEngine::TimeInForce::GTC,
                             .price = 100,
                             .quantity = 5,
                             .side = StpEngine::Side::Sell,
                             .participant = 7},
                            [&](const StpEngine::Fill &f) { fills.push_back(f); });
  ASSERT_TRUE(a.resting());

  // The replacement carries a different participant; modify must ignore it and
  // inherit the original's key.
  const auto r = e.modify(a.orderId,
                          StpEngine::Order{.type = StpEngine::OrderType::Limit,
                                           .tif = StpEngine::TimeInForce::GTC,
                                           .price = 101,
                                           .quantity = 5,
                                           .side = StpEngine::Side::Sell,
                                           .participant = 99},
                          [&](const StpEngine::Fill &f) { fills.push_back(f); });

  EXPECT_EQ(r.result, StpEngine::Result::Resting);
  EXPECT_EQ(e.book().participantOf(r.orderId), 7u); // inherited, not 99
}

TEST(OrderBook, RestOrderDoesNotMatch) {
  Engine e;

  // Feed-driven insert: rest a sell at 100 WITHOUT matching.
  const auto sellId = e.restOrder(100, 10, Side::Sell);
  ASSERT_NE(sellId, Engine::INVALID_ORDER);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.bestAsk(), 100);

  // A buy at 100 would cross in addOrder, but restOrder just rests it.
  const auto buyId = e.restOrder(100, 5, Side::Buy);
  ASSERT_NE(buyId, Engine::INVALID_ORDER);
  EXPECT_EQ(e.openOrders(), 2u);
  EXPECT_EQ(e.bestBid(), 100);
  EXPECT_EQ(e.bestAsk(), 100); // both sides at 100, nothing crossed

  // Execute-by-id reduces the sell.
  EXPECT_TRUE(e.reduce(sellId, 4));
  EXPECT_EQ(e.book().volume(100, Side::Sell), 6u);

  // Cancel the buy by id.
  EXPECT_TRUE(e.cancelOrder(buyId).cancelled);
  EXPECT_EQ(e.openOrders(), 1u);
}

} // namespace
} // namespace flatbook
