#include <gtest/gtest.h>

#include <cstdio>
#include <cstdint>
#include <vector>

#include "flatbook/Lobster.hpp"
#include "flatbook/LobsterValidator.hpp"
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

TEST(Lobster, ParseOrderbookLine) {
  OrderbookLevel levels[4];
  // Official LOBSTER orderbook sample: level 1 = (1186600 x 9484 / 118500 x 8800),
  // level 2 = (118700 x 22700 / 118400 x 14930).
  const auto n = parseOrderbookLine("1186600 9484 118500 8800 118700 22700 118400 14930",
                                    levels, 4);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(levels[0].askPrice, 1186600);
  EXPECT_EQ(levels[0].askSize, 9484u);
  EXPECT_EQ(levels[0].bidPrice, 118500);
  EXPECT_EQ(levels[0].bidSize, 8800u);
  EXPECT_EQ(levels[1].askPrice, 118700);
  EXPECT_EQ(levels[1].askSize, 22700u);
  EXPECT_EQ(levels[1].bidPrice, 118400);
  EXPECT_EQ(levels[1].bidSize, 14930u);
}

TEST(Lobster, ParseOrderbookLineDummyLevel) {
  OrderbookLevel levels[2];
  const auto n = parseOrderbookLine("9999999999 0 -9999999999 0", levels, 2);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(levels[0].askPrice, 9999999999LL);
  EXPECT_EQ(levels[0].askSize, 0u);
  EXPECT_EQ(levels[0].bidPrice, -9999999999LL);
  EXPECT_EQ(levels[0].bidSize, 0u);
}

TEST(Lobster, FeedReplayRestsAndReducesById) {
  Engine e;
  LobsterFeed<Engine> feed(e);

  // Submission rests without matching.
  feed.message(1, 1, 10, 50, false); // sell 10 @ 50
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.bestAsk(), 50);
  EXPECT_EQ(e.bestBid(), Engine::INVALID_PRICE); // no buy side yet

  // A buy at 50 would cross in matching, but the feed just rests it.
  feed.message(1, 2, 5, 50, true);
  EXPECT_EQ(e.openOrders(), 2u);
  EXPECT_EQ(e.bestBid(), 50);
  EXPECT_EQ(e.book().volume(50, Side::Sell), 10u); // sell untouched

  // Execution of 4 against order id 1.
  feed.message(4, 1, 4, 50, false);
  EXPECT_EQ(e.book().volume(50, Side::Sell), 6u);

  // Deletion of order id 2.
  feed.message(3, 2, 0, 50, true);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.book().volume(50, Side::Buy), 0u);
}

TEST(Lobster, FeedReplayRebaseHysteresis) {
  Engine e; // window [0, 64)
  LobsterFeed<Engine> feed(e);

  // A price just above the window shifts it by half a width (not centered).
  feed.message(1, 1, 10, 70, true);
  EXPECT_EQ(feed.stats().rebases, 1u);
  EXPECT_EQ(e.base(), 32); // 0 + 64/2
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.bestBid(), 70);

  // A far price beyond one step is dropped, not snapped-to: the touch survives.
  feed.message(1, 2, 5, 1000, false);
  EXPECT_EQ(feed.stats().rebases, 2u);
  EXPECT_EQ(feed.stats().skipped, 1u);
  EXPECT_EQ(e.openOrders(), 1u);
  EXPECT_EQ(e.bestBid(), 70);
}

TEST(Lobster, FeedReplayTracksRebaseCancellations) {
  Engine e; // window [0, 64)
  LobsterFeed<Engine> feed(e);

  feed.message(1, 1, 10, 5, true);  // rests near the bottom of the window
  EXPECT_EQ(e.openOrders(), 1u);

  // Push the window up by half a width: the bid at 5 falls out.
  feed.message(1, 2, 5, 70, true);
  EXPECT_EQ(feed.stats().cancelledByRebase, 1u);

  // A later reference to the cancelled order is rebasedAway, not skipped.
  feed.message(3, 1, 0, 5, true);
  EXPECT_EQ(feed.stats().rebasedAway, 1u);
  EXPECT_EQ(feed.stats().skipped, 0u);
}

namespace {

using ValBook = OrderBook<256, 256, GenerationalId<256, 24>>; // window [0, 256)
using ValEngine = MatchingEngine<ValBook>;

// Writes a hand-written message + orderbook fixture and runs the validator.
void writeFixture(const char *messagePath, const char *orderbookPath, bool corrupt) {
  std::FILE *mf = std::fopen(messagePath, "w");
  ASSERT_NE(mf, nullptr);
  std::fprintf(mf, "0.0 1 1 10 100 -1\n");  // submit sell 10 @ 100 (id 1)
  std::fprintf(mf, "0.1 1 2 5 99 1\n");     // submit buy 5 @ 99 (id 2)
  std::fprintf(mf, "0.2 4 1 4 100 -1\n");   // execute 4 against id 1
  std::fprintf(mf, "0.3 3 2 0 99 1\n");     // delete id 2
  std::fclose(mf);

  std::FILE *of = std::fopen(orderbookPath, "w");
  ASSERT_NE(of, nullptr);
  std::fprintf(of, "100 10 -9999999999 0\n"); // after msg 1: ask 100x10, no bid
  std::fprintf(of, "100 10 99 5\n");          // after msg 2
  std::fprintf(of, corrupt ? "100 5 99 5\n" : "100 6 99 5\n"); // after msg 3 (corrupt: wrong size)
  std::fprintf(of, "100 6 -9999999999 0\n");  // after msg 4
  std::fclose(of);
}

} // namespace

TEST(Lobster, ValidatorMatchesFixture) {
  const char *mp = "lobster_val_msg.csv";
  const char *op = "lobster_val_ob.csv";
  writeFixture(mp, op, /*corrupt=*/false);

  ValEngine e;
  const auto r = validateLobster(e, mp, op, /*numLevels=*/1);
  std::remove(mp);
  std::remove(op);

  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.messages, 4u);
}

TEST(Lobster, ValidatorDetectsMismatch) {
  const char *mp = "lobster_val_msg.csv";
  const char *op = "lobster_val_ob.csv";
  writeFixture(mp, op, /*corrupt=*/true);

  ValEngine e;
  const auto r = validateLobster(e, mp, op, /*numLevels=*/1);
  std::remove(mp);
  std::remove(op);

  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.line, 3u);       // mismatch on the third message
  EXPECT_EQ(r.level, 1u);
  EXPECT_TRUE(r.askSide);      // ... on the ask side
  EXPECT_EQ(r.expectedSize, 5u);
  EXPECT_EQ(r.actualSize, 6u);
}

} // namespace
} // namespace flatbook
