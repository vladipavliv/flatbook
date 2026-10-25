#include <gtest/gtest.h>

#include <cstdio>
#include <cstdint>
#include <vector>

#include "flatbook/Lobster.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<256, 64, GenerationalId<256, 24>>; // small window; generational ids
using Engine = MatchingEngine<Book>;
using Side = Engine::Side;

TEST(Lobster, ParseLine) {
  int type = 0;
  std::uint64_t orderId = 0;
  std::uint32_t size = 0;
  std::int64_t price = 0;
  bool buy = false;

  // Official LOBSTER sample line (space-separated despite the .csv extension).
  ASSERT_TRUE(parseLobsterLine("34713.685155243 1 206833312 100 118600 -1", type, orderId, size,
                               price, buy));
  EXPECT_EQ(type, 1);
  EXPECT_EQ(orderId, 206833312u);
  EXPECT_EQ(size, 100u);
  EXPECT_EQ(price, 118600); // dollar price x 10000 ($11.86)
  EXPECT_FALSE(buy);        // -1 = sell

  ASSERT_TRUE(parseLobsterLine("34200.2 3 100 0 10000 1", type, orderId, size, price, buy));
  EXPECT_EQ(type, 3);
  EXPECT_EQ(orderId, 100u);
  EXPECT_EQ(size, 0u);
  EXPECT_TRUE(buy);

  EXPECT_FALSE(parseLobsterLine("garbage", type, orderId, size, price, buy));

  // Comma-separated variant must also parse (some LOBSTER distributions).
  ASSERT_TRUE(parseLobsterLine("34200.3,1,200,3,10005,1", type, orderId, size, price, buy));
  EXPECT_EQ(type, 1);
  EXPECT_EQ(orderId, 200u);
  EXPECT_EQ(size, 3u);
  EXPECT_EQ(price, 10005);
  EXPECT_TRUE(buy);
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

TEST(Lobster, ReplayFileCountsLinesAndParseFailures) {
  const char *path = "lobster_replay_test.csv";
  {
    std::FILE *f = std::fopen(path, "w");
    ASSERT_NE(f, nullptr);
    std::fprintf(f, "1.0 1 1 10 100 1\n");
    std::fprintf(f, "2.0 3 1 0 100 1\n");  // delete id 1
    std::fprintf(f, "3.0 1 2 5 101 -1\n"); // submit id 2
    std::fprintf(f, "garbage line\n");     // parse failure -> skipped
    std::fclose(f);
  }

  Engine e;
  const auto stats = replayLobsterFile(e, path);
  std::remove(path);

  EXPECT_EQ(stats.lines, 4u);
  EXPECT_EQ(stats.submissions, 2u);
  EXPECT_EQ(stats.deletions, 1u);
  EXPECT_EQ(stats.skipped, 1u); // the garbage line
  EXPECT_EQ(e.openOrders(), 1u); // only id 2 (sell @ 101) rests
  EXPECT_EQ(e.bestAsk(), 101);
}

TEST(Lobster, MissingFileReturnsEmptyStats) {
  Engine e;
  const auto stats = replayLobsterFile(e, "lobster_does_not_exist.csv");
  EXPECT_EQ(stats.lines, 0u);
  EXPECT_EQ(stats.submissions, 0u);
}

} // namespace
} // namespace flatbook
