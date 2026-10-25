#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "flatbook/MatchingDispatcher.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<1 << 15, 1 << 10>;
using Engine = MatchingEngine<Book>;
using Order = Engine::Order;
using OrderIdx = Engine::OrderIdx;

Order makeOrder(std::mt19937_64 &rng) {
  Order o;
  o.type = (rng() % 10 == 0) ? Engine::OrderType::Market : Engine::OrderType::Limit;
  o.tif = static_cast<Engine::TimeInForce>(rng() % 3);
  o.price = static_cast<Engine::PriceIdx>(rng() % Book::kPriceLevels);
  o.quantity = static_cast<Quantity>(1 + rng() % 100);
  o.side = (rng() & 1) ? Engine::Side::Buy : Engine::Side::Sell;
  return o;
}

TEST(MatchingDispatcher, MatchesSingleThreadedReference) {
  constexpr size_t kSymbols = 16;
  constexpr size_t kOrdersPerSymbol = 2000;

  std::array<std::vector<Order>, kSymbols> orders;
  std::mt19937_64 rng(42);
  for (size_t s = 0; s < kSymbols; ++s) {
    for (size_t i = 0; i < kOrdersPerSymbol; ++i) {
      orders[s].push_back(makeOrder(rng));
    }
  }

  // Single-threaded reference: replay each symbol independently.
  std::array<std::vector<Engine::Fill>, kSymbols> refFills;
  std::array<std::vector<Engine::AddResult>, kSymbols> refResults;
  for (size_t s = 0; s < kSymbols; ++s) {
    Engine e;
    for (const auto &o : orders[s]) {
      refResults[s].push_back(e.addOrder(o, [&](const Engine::Fill &f) { refFills[s].push_back(f); }));
    }
  }

  // Dispatcher: 4 workers, collect fills/results per symbol from the callback.
  std::array<std::vector<Engine::Fill>, kSymbols> fills;
  std::array<std::vector<Engine::AddResult>, kSymbols> results;

  MatchingConfig config;
  config.workerCores = {0, 1, 2, 3};

  MatchingDispatcher<kSymbols, Book> dispatcher(
      config, [&](const MatchingDispatcher<kSymbols, Book>::Event &ev) {
        if (ev.kind == MatchingDispatcher<kSymbols, Book>::Event::Kind::Fill) {
          fills[ev.symbolIdx].push_back(ev.fill);
        } else {
          results[ev.symbolIdx].push_back(ev.result);
        }
      });

  dispatcher.start();

  // Submit orders interleaved across symbols (single ingress thread).
  for (size_t i = 0; i < kOrdersPerSymbol; ++i) {
    for (size_t s = 0; s < kSymbols; ++s) {
      dispatcher.addOrder(static_cast<uint32_t>(s), orders[s][i]);
    }
  }

  dispatcher.stop();

  for (size_t s = 0; s < kSymbols; ++s) {
    ASSERT_EQ(fills[s].size(), refFills[s].size()) << "symbol " << s;
    for (size_t i = 0; i < fills[s].size(); ++i) {
      EXPECT_EQ(fills[s][i].orderId, refFills[s][i].orderId);
      EXPECT_EQ(fills[s][i].price, refFills[s][i].price);
      EXPECT_EQ(fills[s][i].quantity, refFills[s][i].quantity);
      EXPECT_EQ(fills[s][i].side, refFills[s][i].side);
      EXPECT_EQ(fills[s][i].seq, refFills[s][i].seq);
    }

    ASSERT_EQ(results[s].size(), refResults[s].size()) << "symbol " << s;
    for (size_t i = 0; i < results[s].size(); ++i) {
      EXPECT_EQ(results[s][i].result, refResults[s][i].result);
      EXPECT_EQ(results[s][i].orderId, refResults[s][i].orderId);
      EXPECT_EQ(results[s][i].remaining, refResults[s][i].remaining);
      EXPECT_EQ(results[s][i].seq, refResults[s][i].seq);
    }
  }
}

TEST(MatchingDispatcher, CancelModifyMatchReference) {
  constexpr size_t kSymbols = 8;
  constexpr size_t kOpsPerSymbol = 1000;

  enum class OpKind : uint8_t { Add, Cancel, Modify };
  struct Op {
    OpKind kind{OpKind::Add};
    Order order{};
    OrderIdx id{};
  };

  std::array<std::vector<Op>, kSymbols> ops;
  std::array<std::vector<Engine::Fill>, kSymbols> refFills;
  std::array<std::vector<Engine::AddResult>, kSymbols> refResults;
  std::array<std::vector<bool>, kSymbols> refCancels;

  std::mt19937_64 rng(7);

  // Single-threaded reference replay; also generates the concrete op list
  // (cancel/modify target the reference's live ids, which are deterministic).
  for (size_t s = 0; s < kSymbols; ++s) {
    Engine e;
    std::vector<OrderIdx> live;
    for (size_t i = 0; i < kOpsPerSymbol; ++i) {
      const int r = static_cast<int>(rng() % 100);
      Op op;
      if (r < 70 || live.empty()) {
        op.kind = OpKind::Add;
        op.order = makeOrder(rng);
        ops[s].push_back(op);
        const auto res = e.addOrder(op.order, [&](const Engine::Fill &f) { refFills[s].push_back(f); });
        refResults[s].push_back(res);
        if (res.resting()) {
          live.push_back(res.orderId);
        }
      } else if (r < 85) {
        const size_t idx = static_cast<size_t>(rng() % live.size());
        op.kind = OpKind::Cancel;
        op.id = live[idx];
        ops[s].push_back(op);
        refCancels[s].push_back(e.cancelOrder(op.id));
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
      } else {
        const size_t idx = static_cast<size_t>(rng() % live.size());
        op.kind = OpKind::Modify;
        op.id = live[idx];
        op.order = makeOrder(rng);
        op.order.type = Engine::OrderType::Limit;
        op.order.tif = Engine::TimeInForce::GTC;
        op.order.side = e.book().sideOf(op.id);
        ops[s].push_back(op);
        const auto res = e.modify(op.id, op.order, [&](const Engine::Fill &f) { refFills[s].push_back(f); });
        refResults[s].push_back(res);
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
        if (res.resting()) {
          live.push_back(res.orderId);
        }
      }
    }
  }

  std::array<std::vector<Engine::Fill>, kSymbols> fills;
  std::array<std::vector<Engine::AddResult>, kSymbols> results;
  std::array<std::vector<bool>, kSymbols> cancels;

  MatchingConfig config;
  config.workerCores = {0, 1, 2, 3};

  MatchingDispatcher<kSymbols, Book> dispatcher(
      config, [&](const MatchingDispatcher<kSymbols, Book>::Event &ev) {
        switch (ev.kind) {
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Fill:
          fills[ev.symbolIdx].push_back(ev.fill);
          break;
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Result:
          results[ev.symbolIdx].push_back(ev.result);
          break;
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Cancel:
          cancels[ev.symbolIdx].push_back(ev.cancelled);
          break;
        }
      });

  dispatcher.start();

  for (size_t i = 0; i < kOpsPerSymbol; ++i) {
    for (size_t s = 0; s < kSymbols; ++s) {
      const Op &op = ops[s][i];
      switch (op.kind) {
      case OpKind::Add:
        dispatcher.addOrder(static_cast<uint32_t>(s), op.order);
        break;
      case OpKind::Cancel:
        dispatcher.cancelOrder(static_cast<uint32_t>(s), op.id);
        break;
      case OpKind::Modify:
        dispatcher.modify(static_cast<uint32_t>(s), op.id, op.order);
        break;
      }
    }
  }

  dispatcher.stop();

  for (size_t s = 0; s < kSymbols; ++s) {
    ASSERT_EQ(fills[s].size(), refFills[s].size()) << "symbol " << s;
    for (size_t i = 0; i < fills[s].size(); ++i) {
      EXPECT_EQ(fills[s][i].orderId, refFills[s][i].orderId);
      EXPECT_EQ(fills[s][i].price, refFills[s][i].price);
      EXPECT_EQ(fills[s][i].quantity, refFills[s][i].quantity);
      EXPECT_EQ(fills[s][i].side, refFills[s][i].side);
      EXPECT_EQ(fills[s][i].seq, refFills[s][i].seq);
    }

    ASSERT_EQ(results[s].size(), refResults[s].size()) << "symbol " << s;
    for (size_t i = 0; i < results[s].size(); ++i) {
      EXPECT_EQ(results[s][i].result, refResults[s][i].result);
      EXPECT_EQ(results[s][i].orderId, refResults[s][i].orderId);
      EXPECT_EQ(results[s][i].remaining, refResults[s][i].remaining);
      EXPECT_EQ(results[s][i].seq, refResults[s][i].seq);
    }

    ASSERT_EQ(cancels[s].size(), refCancels[s].size()) << "symbol " << s;
    for (size_t i = 0; i < cancels[s].size(); ++i) {
      EXPECT_EQ(cancels[s][i], refCancels[s][i]);
    }
  }
}

} // namespace
} // namespace flatbook
