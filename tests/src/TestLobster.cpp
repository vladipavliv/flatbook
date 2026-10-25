#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "flatbook/Lobster.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<256, 64>; // small window, so the replay exercises rebase
using Engine = MatchingEngine<Book>;
using Side = Engine::Side;

TEST(Lobster, ParseLine) {
  int type = 0;
  std::uint64_t orderId = 0;
  std::uint32_t size = 0;
  std::int64_t price = 0;
  bool buy = false;

  ASSERT_TRUE(parseLobsterLine("34200.1 1 100 50 10000 -1", type, orderId, size, price, buy));
  EXPECT_EQ(type, 1);
  EXPECT_EQ(orderId, 100u);
  EXPECT_EQ(size, 50u);
  EXPECT_EQ(price, 10000);
  EXPECT_FALSE(buy); // -1 = sell

  ASSERT_TRUE(parseLobsterLine("34200.2 3 100 0 10000 1", type, orderId, size, price, buy));
  EXPECT_EQ(type, 3);
  EXPECT_EQ(orderId, 100u);
  EXPECT_EQ(size, 0u);
  EXPECT_TRUE(buy);

  EXPECT_FALSE(parseLobsterLine("garbage", type, orderId, size, price, buy));
}

TEST(Lobster, ReplayCrossesAndDeletes) {
  Engine e;
  LobsterReplay<Engine> replay(e);
  std::vector<Engine::Fill> fills;
  replay.onFill([&](const Engine::Fill &f) { fills.push_back(f); });

  // Submission: sell 10 @ 100 rests (lobster id 1).
  replay.message(1, 1, 10, 100, false);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.bestAsk(), 100);

  // Submission: buy 5 @ 100 crosses the resting sell, fill 5 @ 100.
  replay.message(1, 2, 5, 100, true);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].price, 100);
  EXPECT_EQ(fills[0].quantity, 5u);
  EXPECT_EQ(e.openOrders(), 1u); // sell left with 5

  // Deletion: remove lobster id 1 (the resting sell).
  replay.message(3, 1, 0, 100, true);
  EXPECT_EQ(e.openOrders(), 0u);

  EXPECT_EQ(replay.stats().submissions, 2u);
  EXPECT_EQ(replay.stats().deletions, 1u);
}

TEST(Lobster, ReplayExecutionAndPartialCancel) {
  Engine e;
  LobsterReplay<Engine> replay(e);

  // Submission: sell 10 @ 100 rests (lobster id 7).
  replay.message(1, 7, 10, 100, false);
  EXPECT_EQ(e.book().volume(100, Side::Sell), 10u);

  // Visible execution of 4 against lobster id 7.
  replay.message(4, 7, 4, 100, false);
  EXPECT_EQ(e.book().volume(100, Side::Sell), 6u);
  EXPECT_EQ(e.openOrders(), 1u);

  // Partial cancel of 3 against lobster id 7.
  replay.message(2, 7, 3, 100, false);
  EXPECT_EQ(e.book().volume(100, Side::Sell), 3u);

  // Full deletion removes the rest.
  replay.message(3, 7, 0, 100, false);
  EXPECT_EQ(e.openOrders(), 0u);

  EXPECT_EQ(replay.stats().executions, 1u);
  EXPECT_EQ(replay.stats().partialCancels, 1u);
}

TEST(Lobster, ReplayRebasesWindowToFollowPrice) {
  Engine e;
  LobsterReplay<Engine> replay(e);

  // Price far outside the initial window [0, 64): the replay must rebase.
  replay.message(1, 1, 10, 1000, true);
  EXPECT_EQ(e.base(), 1000 - static_cast<Engine::Price>(Engine::kPriceLevels) / 2);
  EXPECT_EQ(e.bestBid(), 1000);
  EXPECT_EQ(e.openOrders(), 1u);

  // And again on the other side.
  replay.message(1, 2, 5, 5000, false);
  EXPECT_EQ(e.base(), 5000 - static_cast<Engine::Price>(Engine::kPriceLevels) / 2);
  EXPECT_EQ(e.bestAsk(), 5000);
  EXPECT_EQ(e.openOrders(), 1u); // the far bid was cancelled by the rebase
}

} // namespace
} // namespace flatbook
