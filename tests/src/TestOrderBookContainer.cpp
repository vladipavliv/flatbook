#include <gtest/gtest.h>

#include <cstdint>

#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<64, 32>;
using OrderIdx = Book::OrderIdx;
using PriceIdx = Book::PriceIdx;
using Side = Book::Side;

using GenBook = OrderBook<64, 32, GenerationalId<64, 16>>;
using StpBook = OrderBook<64, 32, RawId<64>, std::uint32_t>;
using SmallBook = OrderBook<8, 32>;

// ---------------------------------------------------------------------------
// insert / isLive / remove
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, InsertReturnsLiveId) {
  Book b;
  const OrderIdx id = b.insert(10, 5, Side::Buy);
  EXPECT_NE(id, Book::INVALID_ORDER);
  EXPECT_TRUE(b.isLive(id));
  EXPECT_EQ(b.openOrders(), 1u);
}

TEST(OrderBookContainer, InsertRejectsBadInput) {
  Book b;
  EXPECT_EQ(b.insert(10, 0, Side::Buy), Book::INVALID_ORDER);                 // zero qty
  EXPECT_EQ(b.insert(Book::kPriceLevels, 5, Side::Buy), Book::INVALID_ORDER); // price OOB
  EXPECT_EQ(b.openOrders(), 0u);
}

TEST(OrderBookContainer, RemoveLiveThenStale) {
  Book b;
  const OrderIdx id = b.insert(10, 5, Side::Buy);
  EXPECT_TRUE(b.remove(id));
  EXPECT_FALSE(b.isLive(id));
  EXPECT_FALSE(b.remove(id)); // double remove
  EXPECT_EQ(b.openOrders(), 0u);
}

TEST(OrderBookContainer, RemoveDecrementsOpenOrders) {
  Book b;
  (void)b.insert(10, 5, Side::Buy);
  (void)b.insert(12, 5, Side::Sell);
  EXPECT_EQ(b.openOrders(), 2u);
  EXPECT_TRUE(b.remove(b.head(10, Side::Buy)));
  EXPECT_EQ(b.openOrders(), 1u);
}

// ---------------------------------------------------------------------------
// FIFO queue traversal
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, HeadAndNextAreFifo) {
  Book b;
  const OrderIdx a = b.insert(10, 5, Side::Buy);
  const OrderIdx c = b.insert(10, 7, Side::Buy);
  EXPECT_EQ(b.head(10, Side::Buy), a);
  EXPECT_EQ(b.nextOf(a), c);
  EXPECT_EQ(b.nextOf(c), Book::INVALID_ORDER);
}

TEST(OrderBookContainer, EmptyLevelHasInvalidHead) {
  Book b;
  EXPECT_EQ(b.head(10, Side::Buy), Book::INVALID_ORDER);
  EXPECT_EQ(b.head(10, Side::Sell), Book::INVALID_ORDER);
}

TEST(OrderBookContainer, TraversalSpansEntireQueue) {
  Book b;
  (void)b.insert(10, 1, Side::Buy);
  (void)b.insert(10, 2, Side::Buy);
  (void)b.insert(10, 3, Side::Buy);

  std::size_t n = 0;
  Quantity sum = 0;
  for (OrderIdx id = b.head(10, Side::Buy); id != Book::INVALID_ORDER; id = b.nextOf(id)) {
    ++n;
    sum += b.qty(id);
  }
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(sum, 1u + 2u + 3u);
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, VolumeAggregatesAtLevel) {
  Book b;
  (void)b.insert(10, 5, Side::Buy);
  (void)b.insert(10, 7, Side::Buy);
  (void)b.insert(10, 3, Side::Sell);
  EXPECT_EQ(b.volume(10, Side::Buy), 12u);
  EXPECT_EQ(b.volume(10, Side::Sell), 3u);
  EXPECT_EQ(b.volume(11, Side::Buy), 0u);
}

// ---------------------------------------------------------------------------
// Best bid / ask
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, BestBidBestAskWithGaps) {
  Book b;
  (void)b.insert(10, 1, Side::Buy);
  (void)b.insert(12, 1, Side::Buy);
  (void)b.insert(20, 1, Side::Sell);
  (void)b.insert(25, 1, Side::Sell);
  EXPECT_EQ(b.bestBid(), 12u);
  EXPECT_EQ(b.bestAsk(), 20u);
}

TEST(OrderBookContainer, BestBidUpdatesOnRemove) {
  Book b;
  const OrderIdx top = b.insert(12, 1, Side::Buy);
  (void)b.insert(10, 1, Side::Buy);
  EXPECT_EQ(b.bestBid(), 12u);
  EXPECT_TRUE(b.remove(top));
  EXPECT_EQ(b.bestBid(), 10u);
}

TEST(OrderBookContainer, EmptyBookHasInvalidBest) {
  Book b;
  EXPECT_EQ(b.bestBid(), Book::INVALID_PRICE);
  EXPECT_EQ(b.bestAsk(), Book::INVALID_PRICE);
}

// ---------------------------------------------------------------------------
// nextLevel (level iteration)
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, NextLevelBidWalksDown) {
  Book b;
  (void)b.insert(5, 1, Side::Buy);
  (void)b.insert(12, 1, Side::Buy);
  (void)b.insert(20, 1, Side::Buy);
  EXPECT_EQ(b.nextLevel(0, Side::Buy), Book::INVALID_PRICE); // none <= 0
  EXPECT_EQ(b.nextLevel(5, Side::Buy), 5u);
  EXPECT_EQ(b.nextLevel(6, Side::Buy), 5u); // highest <= 6
  EXPECT_EQ(b.nextLevel(19, Side::Buy), 12u);
  EXPECT_EQ(b.nextLevel(20, Side::Buy), 20u);
  EXPECT_EQ(b.nextLevel(21, Side::Buy), 20u);
}

TEST(OrderBookContainer, NextLevelAskWalksUp) {
  Book b;
  (void)b.insert(7, 1, Side::Sell);
  (void)b.insert(15, 1, Side::Sell);
  (void)b.insert(25, 1, Side::Sell);
  EXPECT_EQ(b.nextLevel(0, Side::Sell), 7u);
  EXPECT_EQ(b.nextLevel(8, Side::Sell), 15u);
  EXPECT_EQ(b.nextLevel(25, Side::Sell), 25u);
  EXPECT_EQ(b.nextLevel(26, Side::Sell), Book::INVALID_PRICE);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, AccessorsReportOrderState) {
  Book b;
  const OrderIdx id = b.insert(10, 7, Side::Sell);
  EXPECT_EQ(b.qty(id), 7u);
  EXPECT_EQ(b.priceOf(id), 10u);
  EXPECT_EQ(b.sideOf(id), Side::Sell);
}

TEST(OrderBookContainer, OrderNodeView) {
  Book b;
  const OrderIdx id = b.insert(10, 7, Side::Sell);
  const auto &node = b.order(id);
  EXPECT_EQ(node.qty, 7u);
  EXPECT_EQ(node.price, 10u);
  EXPECT_EQ(node.side, Side::Sell);
}

TEST(OrderBookContainer, LevelView) {
  Book b;
  (void)b.insert(10, 7, Side::Buy);
  const auto &level = b.level(10);
  EXPECT_EQ(level.bid.volume, 7u);
  EXPECT_EQ(level.ask.volume, 0u);
}

// ---------------------------------------------------------------------------
// Capacity / recycling
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, CapacityExhaustion) {
  SmallBook small;
  for (int i = 0; i < 7; ++i) {
    EXPECT_NE(small.insert(10, 1, SmallBook::Side::Buy), SmallBook::INVALID_ORDER) << "order " << i;
  }
  EXPECT_EQ(small.insert(10, 1, SmallBook::Side::Buy), SmallBook::INVALID_ORDER);
  EXPECT_EQ(small.openOrders(), 7u);
}

TEST(OrderBookContainer, ReleasedSlotReusedLifo) {
  Book b;
  const OrderIdx a = b.insert(10, 1, Side::Buy);
  EXPECT_TRUE(b.remove(a));
  const OrderIdx c = b.insert(11, 1, Side::Buy);
  EXPECT_EQ(c.index(), a.index());
}

// ---------------------------------------------------------------------------
// Generational id (ABA protection at the container level)
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, GenerationalIdRejectsStaleRemove) {
  GenBook b;
  const auto a = b.insert(10, 1, GenBook::Side::Buy);
  EXPECT_TRUE(b.remove(a));

  const auto c = b.insert(10, 1, GenBook::Side::Buy); // reuses the same slot
  EXPECT_EQ(c.index(), a.index());
  EXPECT_NE(c.generation(), a.generation());

  EXPECT_FALSE(b.remove(a)); // stale generation must be rejected
  EXPECT_TRUE(b.isLive(c));
  EXPECT_EQ(b.openOrders(), 1u);
}

// ---------------------------------------------------------------------------
// Self-trade-prevention participant keys
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, ParticipantStoredAndReadable) {
  StpBook b;
  const auto id = b.insert(10, 5, StpBook::Side::Buy, 7u);
  EXPECT_EQ(b.participantOf(id), 7u);
  EXPECT_TRUE(StpBook::kHasParticipants);
}

TEST(OrderBookContainer, ParticipantDefaultsToZeroWhenDisabled) {
  Book b; // Participant = void
  const auto id = b.insert(10, 5, Side::Buy);
  EXPECT_EQ(b.participantOf(id), 0u);
  EXPECT_FALSE(Book::kHasParticipants);
}

// ---------------------------------------------------------------------------
// clear
// ---------------------------------------------------------------------------

TEST(OrderBookContainer, ClearEmptiesEverything) {
  Book b;
  const OrderIdx a = b.insert(10, 5, Side::Buy);
  (void)b.insert(12, 5, Side::Sell);
  b.clear();
  EXPECT_EQ(b.openOrders(), 0u);
  EXPECT_EQ(b.bestBid(), Book::INVALID_PRICE);
  EXPECT_EQ(b.bestAsk(), Book::INVALID_PRICE);
  EXPECT_FALSE(b.isLive(a));
}

} // namespace
} // namespace flatbook
