#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "flatbook/Journal.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"
#include "flatbook/Persist.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<2048, 64, GenerationalId<2048, 24>, std::uint32_t>;
using Engine = MatchingEngine<Book>;
using Order = Engine::Order;
using OrderIdx = Engine::OrderIdx;
using Price = Engine::Price;

// Drive a deterministic random op stream (add/cancel/modify/rebase), recording
// to the journal. `live` tracks resting order ids across calls.
void drive(Engine &e, Journal<Engine> &j, std::vector<OrderIdx> &live, std::mt19937_64 &rng,
           Price &base, int ops) {
  for (int i = 0; i < ops; ++i) {
    const int r = static_cast<int>(rng() % 100);
    std::vector<Engine::Fill> fills;

    if (r < 8 && !live.empty()) {
      const std::size_t idx = static_cast<std::size_t>(rng() % live.size());
      const auto id = live[idx];
      j.recordCancel(id, e.cancelOrder(id));
      live[idx] = live.back();
      live.pop_back();
    } else if (r < 12) {
      const Price nb = base + static_cast<Price>(rng() % 40) - 20;
      j.recordRebase(nb, e.rebase(nb));
      base = nb;
      std::vector<OrderIdx> pruned;
      pruned.reserve(live.size());
      for (const auto id : live) {
        if (e.book().isLive(id)) {
          pruned.push_back(id);
        }
      }
      live.swap(pruned);
    } else if (r < 20 && !live.empty()) {
      const std::size_t idx = static_cast<std::size_t>(rng() % live.size());
      const auto id = live[idx];
      Order repl{};
      repl.type = Engine::OrderType::Limit;
      repl.tif = Engine::TimeInForce::GTC;
      repl.side = e.book().sideOf(id);
      repl.price = base + static_cast<Price>(rng() % Book::kPriceLevels);
      repl.quantity = static_cast<Quantity>(1 + rng() % 50);
      repl.participant = e.book().participantOf(id);
      const auto res = e.modify(id, repl, [&](const Engine::Fill &f) { fills.push_back(f); });
      j.recordModify(id, repl, res, fills);
      live[idx] = live.back();
      live.pop_back();
      if (res.resting()) {
        live.push_back(res.orderId);
      }
    } else {
      Order o{};
      const int t = static_cast<int>(rng() % 100);
      o.type = (t < 12) ? Engine::OrderType::Market : Engine::OrderType::Limit;
      o.tif = static_cast<Engine::TimeInForce>(rng() % 3);
      o.price = base + static_cast<Price>(rng() % Book::kPriceLevels);
      o.quantity = static_cast<Quantity>(1 + rng() % 50);
      o.side = (rng() & 1) ? Engine::Side::Buy : Engine::Side::Sell;
      o.participant = static_cast<std::uint32_t>(1 + rng() % 4);
      const auto res = e.addOrder(o, [&](const Engine::Fill &f) { fills.push_back(f); });
      j.recordAdd(o, res, fills);
      if (res.resting()) {
        live.push_back(res.orderId);
      }
    }
  }
}

void expectSame(const Engine &a, const Engine &b) {
  ASSERT_EQ(a.base(), b.base());
  ASSERT_EQ(a.openOrders(), b.openOrders());
  ASSERT_EQ(a.lastSequence(), b.lastSequence());
  ASSERT_EQ(a.bestBid(), b.bestBid());
  ASSERT_EQ(a.bestAsk(), b.bestAsk());

  const Price base = a.base();
  for (std::uint32_t s = 0; s < Book::kPriceLevels; ++s) {
    const Price p = base + static_cast<Price>(s);
    for (const auto side : {Engine::Side::Buy, Engine::Side::Sell}) {
      ASSERT_EQ(a.book().volume(p, side), b.book().volume(p, side));
      auto ia = a.book().head(p, side);
      auto ib = b.book().head(p, side);
      while (ia != Engine::INVALID_ORDER && ib != Engine::INVALID_ORDER) {
        ASSERT_EQ(ia, ib);
        ASSERT_EQ(a.book().qty(ia), b.book().qty(ib));
        ASSERT_EQ(a.book().priceOf(ia), b.book().priceOf(ib));
        ia = a.book().nextOf(ia);
        ib = b.book().nextOf(ib);
      }
      ASSERT_EQ(ia == Engine::INVALID_ORDER, ib == Engine::INVALID_ORDER);
    }
  }
}

TEST(Persist, SnapshotPlusTailJournalRoundTrips) {
  const char *path = "flatbook_persist_roundtrip.bin";
  Engine a;
  Journal<Engine> head;
  Journal<Engine> tail;
  std::vector<OrderIdx> live;
  std::mt19937_64 rng(0xF00D);
  Price base = 0;

  drive(a, head, live, rng, base, 4000); // ops baked into the snapshot
  const auto snap = a.snapshot();
  drive(a, tail, live, rng, base, 4000); // ops recorded after the snapshot

  ASSERT_TRUE(saveToFile(path, snap, tail));

  Engine b;
  ASSERT_TRUE(loadFromFile(path, b));
  expectSame(a, b);
  std::remove(path);
}

TEST(Persist, SnapshotOnlyRoundTrips) {
  const char *path = "flatbook_persist_snapshot.bin";
  Engine a;
  Journal<Engine> journal;
  std::vector<OrderIdx> live;
  std::mt19937_64 rng(0xABCD);
  Price base = 0;
  drive(a, journal, live, rng, base, 5000);

  const Journal<Engine> empty;
  ASSERT_TRUE(saveToFile(path, a.snapshot(), empty));

  Engine b;
  ASSERT_TRUE(loadFromFile(path, b));
  expectSame(a, b);
  std::remove(path);
}

TEST(Persist, ConfigMismatchRejected) {
  const char *path = "flatbook_persist_mismatch.bin";
  using PlainEngine = MatchingEngine<OrderBook<1024, 64>>; // no participants
  PlainEngine a;
  Journal<PlainEngine> j;
  ASSERT_TRUE(saveToFile(path, a.snapshot(), j));

  // Same Size/PriceLevels but different participant config -> must be rejected.
  using StpEngine = MatchingEngine<OrderBook<1024, 64, RawId<1024>, std::uint32_t>>;
  StpEngine b;
  EXPECT_FALSE(loadFromFile(path, b));
  std::remove(path);
}

TEST(Persist, MissingFileFails) {
  Engine e;
  EXPECT_FALSE(loadFromFile("flatbook_does_not_exist.bin", e));
}

TEST(Persist, CorruptFileFailsCleanly) {
  const char *path = "flatbook_persist_corrupt.bin";
  Engine a;
  Journal<Engine> journal;
  std::vector<OrderIdx> live;
  std::mt19937_64 rng(0xBEEF);
  Price base = 0;
  drive(a, journal, live, rng, base, 1000);
  ASSERT_TRUE(saveToFile(path, a.snapshot(), journal));

  // Flip a byte in the payload (header is 32 bytes); the checksum must catch it.
  {
    std::vector<std::uint8_t> buf;
    std::FILE *f = std::fopen(path, "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    buf.resize(static_cast<std::size_t>(size));
    ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
    std::fclose(f);
    ASSERT_GT(buf.size(), 40u);
    buf[40] ^= 0xFF;
    std::FILE *f2 = std::fopen(path, "wb");
    ASSERT_NE(f2, nullptr);
    ASSERT_EQ(std::fwrite(buf.data(), 1, buf.size(), f2), buf.size());
    std::fclose(f2);
  }

  Engine b;
  EXPECT_FALSE(loadFromFile(path, b));
  std::remove(path);
}

TEST(Persist, WideParticipantRoundTrips) {
  const char *path = "flatbook_persist_wide.bin";
  using WideBook = OrderBook<2048, 64, GenerationalId<2048, 24>, std::uint64_t>;
  using WideEngine = MatchingEngine<WideBook>;
  WideEngine a;
  const auto snap = a.snapshot(); // checkpoint before the operation
  Journal<WideEngine> journal;

  WideEngine::Order o{};
  o.type = WideEngine::OrderType::Limit;
  o.tif = WideEngine::TimeInForce::GTC;
  o.price = 10;
  o.quantity = 5;
  o.side = WideEngine::Side::Sell;
  o.participant = 0xDEADBEEFCAFEF00Dull;

  std::vector<WideEngine::Fill> fills;
  const auto r = a.addOrder(o, [&](const WideEngine::Fill &f) { fills.push_back(f); });
  ASSERT_TRUE(r.resting());
  journal.recordAdd(o, r, fills);

  ASSERT_TRUE(saveToFile(path, snap, journal));

  WideEngine b;
  ASSERT_TRUE(loadFromFile(path, b));
  EXPECT_EQ(b.openOrders(), 1u);
  EXPECT_EQ(b.bestAsk(), 10);
  const auto id = b.book().head(10, WideEngine::Side::Sell);
  EXPECT_EQ(b.book().participantOf(id), 0xDEADBEEFCAFEF00Dull);
  std::remove(path);
}

TEST(Persist, BadMagicRejected) {
  const char *path = "flatbook_persist_badmagic.bin";
  {
    std::FILE *f = std::fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    const std::uint32_t bad = 0x12345678;
    ASSERT_EQ(std::fwrite(&bad, 1, sizeof(bad), f), sizeof(bad));
    std::fclose(f);
  }
  Engine e;
  EXPECT_FALSE(loadFromFile(path, e));
  std::remove(path);
}

TEST(Persist, TruncatedHeaderRejected) {
  const char *path = "flatbook_persist_trunc.bin";
  {
    std::FILE *f = std::fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    // "FLBK" magic + one byte of the version field, then nothing: the header
    // is incomplete, so load must fail cleanly.
    const std::uint8_t junk[] = {0x46, 0x4C, 0x42, 0x4B, 0x01};
    ASSERT_EQ(std::fwrite(junk, 1, sizeof(junk), f), sizeof(junk));
    std::fclose(f);
  }
  Engine e;
  EXPECT_FALSE(loadFromFile(path, e));
  std::remove(path);
}

TEST(Persist, LoadDoesNotTouchEngineOnFailure) {
  const char *path = "flatbook_persist_badmagic2.bin";
  {
    std::FILE *f = std::fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    const std::uint32_t bad = 0xDEADBEEF;
    ASSERT_EQ(std::fwrite(&bad, 1, sizeof(bad), f), sizeof(bad));
    std::fclose(f);
  }
  Engine e;
  Engine::Order o{};
  o.type = Engine::OrderType::Limit;
  o.tif = Engine::TimeInForce::GTC;
  o.price = 10;
  o.quantity = 1;
  o.side = Engine::Side::Buy;
  (void)e.addOrder(o);
  const auto before = e.openOrders();

  EXPECT_FALSE(loadFromFile(path, e));
  EXPECT_EQ(e.openOrders(), before); // engine untouched on a bad file
  std::remove(path);
}

} // namespace
} // namespace flatbook
