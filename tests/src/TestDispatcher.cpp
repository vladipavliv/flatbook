#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "flatbook/MatchingDispatcher.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {
namespace {

using Book = OrderBook<1 << 15, 1 << 10, GenerationalId<1 << 15, 24>>;
using Engine = MatchingEngine<Book>;
using Order = Engine::Order;
using OrderIdx = Engine::OrderIdx;

Order makeOrder(std::mt19937_64 &rng) {
  Order o;
  o.type = (rng() % 10 == 0) ? Engine::OrderType::Market : Engine::OrderType::Limit;
  o.tif = static_cast<Engine::TimeInForce>(rng() % 3);
  o.price = static_cast<Engine::Price>(rng() % Book::kPriceLevels);
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

  auto dispatcher = makeDispatcher<kSymbols, Book>(
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
        refCancels[s].push_back(e.cancelOrder(op.id).cancelled);
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

  auto dispatcher = makeDispatcher<kSymbols, Book>(
      config, [&](const MatchingDispatcher<kSymbols, Book>::Event &ev) {
        switch (ev.kind) {
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Fill:
          fills[ev.symbolIdx].push_back(ev.fill);
          break;
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Result:
          results[ev.symbolIdx].push_back(ev.result);
          break;
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Cancel:
          cancels[ev.symbolIdx].push_back(ev.cancel.cancelled);
          break;
        case MatchingDispatcher<kSymbols, Book>::Event::Kind::Rebase:
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

TEST(MatchingDispatcher, UsesConfiguredWorkerCount) {
  MatchingConfig config;
  config.workerCores = {0, 1, 2, 3};
  auto dispatcher = makeDispatcher<4, Book>(config, [](const auto &) {});
  EXPECT_EQ(dispatcher.workerCount(), 4u);
}

TEST(MatchingDispatcher, StopDrainsPendingOrders) {
  constexpr size_t kOrders = 100000;
  MatchingConfig config;
  config.workerCores = {0, 1, 2, 3};

  std::atomic<size_t> results{0};
  auto dispatcher = makeDispatcher<16, Book>(config, [&](const MatchingDispatcher<16, Book>::Event &ev) {
    if (ev.kind == MatchingDispatcher<16, Book>::Event::Kind::Result) {
      results.fetch_add(1, std::memory_order_relaxed);
    }
  });

  dispatcher.start();
  std::mt19937_64 rng(1);
  for (size_t i = 0; i < kOrders; ++i) {
    dispatcher.addOrder(static_cast<uint32_t>(rng() % 16), makeOrder(rng));
  }
  dispatcher.stop();

  EXPECT_EQ(results.load(std::memory_order_relaxed), kOrders);
}

TEST(MatchingDispatcher, EventsCarryCorrelationId) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  std::atomic<bool> gotResult{false};
  std::atomic<bool> gotCancel{false};
  std::uint64_t resultCorr = 0;
  std::uint64_t cancelCorr = 0;
  OrderIdx restingId{};

  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [&](const Dispatcher::Event &ev) {
    if (ev.kind == Dispatcher::Event::Kind::Result) {
      resultCorr = ev.correlationId;
      restingId = ev.result.orderId;
      gotResult.store(true, std::memory_order_release);
    } else if (ev.kind == Dispatcher::Event::Kind::Cancel) {
      cancelCorr = ev.correlationId;
      gotCancel.store(true, std::memory_order_release);
    }
  });

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 100;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;

  dispatcher.start();
  dispatcher.addOrder(0, buy, 42);

  for (int i = 0; i < 1000000 && !gotResult.load(std::memory_order_acquire); ++i) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(gotResult.load(std::memory_order_acquire));

  dispatcher.cancelOrder(0, restingId, 43);
  for (int i = 0; i < 1000000 && !gotCancel.load(std::memory_order_acquire); ++i) {
    std::this_thread::yield();
  }
  dispatcher.stop();

  EXPECT_EQ(resultCorr, 42u);
  EXPECT_EQ(cancelCorr, 43u);
}

TEST(MatchingDispatcher, TrySubmitReportsFull) {
  using Dispatcher = MatchingDispatcher<1, Book, 1 << 4>; // small input queue
  std::atomic<size_t> results{0};
  auto dispatcher = makeDispatcher<1, Book, 1 << 4>({.workerCores = {}}, [&](const Dispatcher::Event &ev) {
    if (ev.kind == Dispatcher::Event::Kind::Result) {
      results.fetch_add(1, std::memory_order_relaxed);
    }
  });

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 100;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;

  // Not started: the worker is not draining, so the input queue fills up.
  size_t accepted = 0;
  bool rejected = false;
  for (int i = 0; i < 100000; ++i) {
    if (dispatcher.tryAddOrder(0, buy, 0)) {
      ++accepted;
    } else {
      rejected = true;
      break;
    }
  }
  EXPECT_TRUE(rejected);
  EXPECT_GT(accepted, 0u);
  EXPECT_GT(dispatcher.stats().submitted.load(), 0u);
  EXPECT_GT(dispatcher.stats().rejected.load(), 0u);

  dispatcher.start();
  dispatcher.stop();
  EXPECT_EQ(results.load(), accepted);
}

TEST(MatchingDispatcher, StatsCountEvents) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [](const Dispatcher::Event &) {});

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 100;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;
  Order sell{};
  sell.type = Engine::OrderType::Limit;
  sell.tif = Engine::TimeInForce::GTC;
  sell.price = 100;
  sell.quantity = 1;
  sell.side = Engine::Side::Sell;

  dispatcher.start();
  dispatcher.addOrder(0, buy, 0);  // rests -> one Result
  dispatcher.addOrder(0, sell, 0); // crosses -> one Fill + one Result
  dispatcher.stop();

  EXPECT_EQ(dispatcher.stats().submitted.load(), 2u);
  EXPECT_EQ(dispatcher.stats().results.load(), 2u);
  EXPECT_EQ(dispatcher.stats().fills.load(), 1u);
  EXPECT_EQ(dispatcher.stats().cancels.load(), 0u);
}

TEST(MatchingDispatcher, CallbackExceptionIsCaptured) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  std::atomic<int> calls{0};
  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [&](const Dispatcher::Event &) {
    calls.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("boom");
  });

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 100;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;

  dispatcher.start();
  dispatcher.addOrder(0, buy, 0);
  dispatcher.addOrder(0, buy, 0);
  dispatcher.stop(); // must not std::terminate

  EXPECT_NE(dispatcher.callbackError(), nullptr);
  EXPECT_GE(calls.load(), 1);
}

TEST(MatchingDispatcher, CancelEventCarriesIdAndSeq) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  std::atomic<bool> gotCancel{false};
  OrderIdx cancelledId{};
  bool hadSeq = false;

  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [&](const Dispatcher::Event &ev) {
    if (ev.kind == Dispatcher::Event::Kind::Cancel) {
      cancelledId = ev.cancel.orderId;
      hadSeq = (ev.cancel.seq != 0);
      gotCancel.store(true, std::memory_order_release);
    }
  });

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 10;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;

  dispatcher.start();
  dispatcher.addOrder(0, buy, 0);
  dispatcher.cancelOrder(0, OrderIdx::fromIndexAndGen(1, 0), 0); // first resting order
  for (int i = 0; i < 1000000 && !gotCancel.load(std::memory_order_acquire); ++i) {
    std::this_thread::yield();
  }
  dispatcher.stop();

  ASSERT_TRUE(gotCancel.load(std::memory_order_acquire));
  EXPECT_EQ(cancelledId.index(), 1u);
  EXPECT_TRUE(hadSeq);
}

TEST(MatchingDispatcher, RebaseCommandEmitsCancelAndRebase) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  std::atomic<int> rebaseCount{0};
  std::atomic<int> cancelCount{0};

  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [&](const Dispatcher::Event &ev) {
    if (ev.kind == Dispatcher::Event::Kind::Rebase) {
      rebaseCount.fetch_add(1, std::memory_order_relaxed);
    } else if (ev.kind == Dispatcher::Event::Kind::Cancel) {
      cancelCount.fetch_add(1, std::memory_order_relaxed);
    }
  });

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 10;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;

  dispatcher.start();
  dispatcher.addOrder(0, buy, 0);
  dispatcher.rebase(0, 1000, 77); // window moves past the resting order
  dispatcher.stop();

  EXPECT_EQ(rebaseCount.load(), 1);
  EXPECT_EQ(cancelCount.load(), 1); // the resting order was cancelled
}

TEST(MatchingDispatcher, InvalidSymbolRejected) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [](const Dispatcher::Event &) {});

  Order buy{};
  buy.type = Engine::OrderType::Limit;
  buy.tif = Engine::TimeInForce::GTC;
  buy.price = 10;
  buy.quantity = 1;
  buy.side = Engine::Side::Buy;

  dispatcher.start();
  EXPECT_FALSE(dispatcher.tryAddOrder(1, buy, 0)); // symbol 1 out of range
  EXPECT_THROW(dispatcher.addOrder(1, buy, 0), std::out_of_range);
  dispatcher.stop();
}

TEST(MatchingDispatcher, StartStopAreIdempotent) {
  using Dispatcher = MatchingDispatcher<1, Book>;
  auto dispatcher = makeDispatcher<1, Book>({.workerCores = {}}, [](const Dispatcher::Event &) {});

  dispatcher.start();
  dispatcher.start(); // second start is a no-op
  dispatcher.stop();
  dispatcher.stop(); // second stop is a no-op
  dispatcher.stop(); // stop without start is a no-op
}

TEST(MatchingDispatcher, NoLostWakeupUnderRepeatedSubmit) {
  constexpr size_t kSymbols = 1;
  std::atomic<std::uint64_t> results{0};

  MatchingConfig config; // empty workerCores => one worker, no pinning
  auto dispatcher = makeDispatcher<kSymbols, Book>(
      config, [&](const MatchingDispatcher<kSymbols, Book>::Event &ev) {
        if (ev.kind == MatchingDispatcher<kSymbols, Book>::Event::Kind::Result) {
          results.fetch_add(1, std::memory_order_release);
        }
      });
  dispatcher.start();

  Order o{};
  o.type = Engine::OrderType::Limit;
  o.tif = Engine::TimeInForce::GTC;
  o.price = 100;
  o.quantity = 1;
  o.side = Engine::Side::Buy;

  constexpr int kIterations = 20000;
  std::uint64_t expected = 0;
  for (int i = 0; i < kIterations; ++i) {
    dispatcher.addOrder(0, o);
    ++expected;
    // Wait for the Result with a timeout: a lost wakeup would stall here.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (results.load(std::memory_order_acquire) < expected) {
      if (std::chrono::steady_clock::now() > deadline) {
        dispatcher.stop();
        FAIL() << "lost wakeup at iteration " << i;
      }
      std::this_thread::yield();
    }
    // Small pause so the worker/collector can park before the next submit.
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  dispatcher.stop();
  EXPECT_EQ(results.load(std::memory_order_acquire), expected);
}

} // namespace
} // namespace flatbook
