#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "BenchCommon.hpp"
#include "Generator.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "reference/HashMapBook.hpp"
#include "reference/MapBook.hpp"

namespace flatbook {

// ---------------------------------------------------------------------------
// Latency of addOrder (Limit GTC) on a warm book. One operation per iteration.
// ---------------------------------------------------------------------------
template <std::uint32_t BookSize, std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_Latency(benchmark::State &state) {
  using Engine = MatchingEngine<OrderBook<BookSize, PriceLevels>>;
  const auto &orders = cachedOrders<Dist, PriceLevels>();

  auto book = std::make_unique<Engine>();

  std::size_t idx = 0;
  std::uint64_t total = 0, resting = 0, filled = 0, rejected = 0;
  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      addLimit(*book, orders[i]);
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book->clear();
      warmUp(kWarmUpCount);
      state.ResumeTiming();
    }

    const auto &o = orders[idx++];
    benchmark::DoNotOptimize(&o);
    const auto r = addLimit(*book, o);
    ++total;
    switch (r.result) {
    case Engine::Result::Resting:
      ++resting;
      break;
    case Engine::Result::Filled:
      ++filled;
      break;
    default:
      ++rejected;
      break;
    }
    benchmark::DoNotOptimize(&r);
    benchmark::ClobberMemory();
  }

  state.counters["restPct"] = 100.0 * static_cast<double>(resting) / static_cast<double>(total);
  state.counters["fillPct"] = 100.0 * static_cast<double>(filled) / static_cast<double>(total);
  state.counters["rejectPct"] = 100.0 * static_cast<double>(rejected) / static_cast<double>(total);
}

// ---------------------------------------------------------------------------
// Latency of an add/cancel mix. The book stays churned at a steady depth, so
// this is the cleanest probe for "latency vs configured capacity": the live
// working set is held roughly constant while BookSize/PriceLevels grow.
// ---------------------------------------------------------------------------
template <std::uint32_t BookSize, std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_Cancel(benchmark::State &state) {
  // Generational ids so a stale (already-filled) id never aliases a recycled
  // slot: cancels of dead ids are no-ops, matching the monotonic-id map baseline.
  using Engine = MatchingEngine<OrderBook<BookSize, PriceLevels, GenerationalId<BookSize, 32>>>;
  const auto &orders = cachedOrders<Dist, PriceLevels>();
  const int nominalCancelPct = static_cast<int>(state.range(0));

  // Precompute the add/cancel pattern and a random cancel target so no RNG
  // runs in the hot loop.
  std::vector<bool> pattern(orders.size());
  std::vector<std::uint32_t> rnd(orders.size());
  {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> chance(0, 99);
    std::uniform_int_distribution<std::uint32_t> rndDist;
    for (std::size_t i = 0; i < orders.size(); ++i) {
      pattern[i] = chance(rng) < nominalCancelPct;
      rnd[i] = rndDist(rng);
    }
  }

  auto book = std::make_unique<Engine>();
  std::vector<typename Engine::OrderIdx> activeIds;
  activeIds.reserve(orders.size());

  std::size_t idx = 0;
  std::size_t patternIdx = 0;
  std::uint64_t adds = 0, cancels = 0, rejected = 0;

  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto r = addLimit(*book, orders[i]);
      if (r.resting()) {
        activeIds.push_back(r.orderId);
      }
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book->clear();
      activeIds.clear();
      warmUp(kWarmUpCount);
      patternIdx = 0;
      state.ResumeTiming();
    }

    const bool wantCancel = pattern[patternIdx];
    ++patternIdx;
    if (patternIdx >= pattern.size()) {
      patternIdx = 0;
    }

    const bool doCancel = wantCancel && !activeIds.empty();

    if (doCancel) {
      // Cancel a random live order (not always the hottest slot).
      const std::size_t pos =
          (static_cast<std::uint64_t>(rnd[patternIdx]) * activeIds.size()) >> 32;
      const auto target = activeIds[pos];
      activeIds[pos] = activeIds.back();
      activeIds.pop_back();
      cancels += book->cancelOrder(target) ? 1 : 0;
    } else {
      const auto &o = orders[idx++];
      benchmark::DoNotOptimize(&o);
      const auto r = addLimit(*book, o);
      if (r.resting()) {
        activeIds.push_back(r.orderId);
      } else if (r.result == Engine::Result::Rejected) {
        ++rejected;
      }
      benchmark::DoNotOptimize(&r);
      ++adds;
    }
    benchmark::ClobberMemory();
  }

  const double total = static_cast<double>(adds + cancels);
  state.counters["addPct"] = 100.0 * static_cast<double>(adds) / total;
  state.counters["cancelPct"] = 100.0 * static_cast<double>(cancels) / total;
  state.counters["rejectPct"] = 100.0 * static_cast<double>(rejected) / total;
}

// ---------------------------------------------------------------------------
// Resting-only add: every order rests (all buys), so the map baseline pays a
// tree insert + allocation on every operation. FlatBook just writes a slot.
// ---------------------------------------------------------------------------
template <std::uint32_t BookSize, std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_RestLatency(benchmark::State &state) {
  using Engine = MatchingEngine<OrderBook<BookSize, PriceLevels>>;
  const auto &orders = cachedOrders<Dist, PriceLevels>();

  auto book = std::make_unique<Engine>();

  std::size_t idx = 0;
  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      addRest(*book, orders[i]);
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book->clear();
      warmUp(kWarmUpCount);
      state.ResumeTiming();
    }
    const auto &o = orders[idx++];
    benchmark::DoNotOptimize(&o);
    const auto r = addRest(*book, o);
    benchmark::DoNotOptimize(&r);
    benchmark::ClobberMemory();
  }
}

template <std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_MapRestLatency(benchmark::State &state) {
  const auto &orders = cachedOrders<Dist, PriceLevels>();

  MapBook book;
  std::size_t idx = 0;
  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto &o = orders[i];
      book.addLimit(o.price, o.qty, true); // force buy: always rests
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book = MapBook{};
      warmUp(kWarmUpCount);
      state.ResumeTiming();
    }
    const auto &o = orders[idx++];
    benchmark::DoNotOptimize(&o);
    book.addLimit(o.price, o.qty, true);
    benchmark::ClobberMemory();
  }
}

template <std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_MapCancel(benchmark::State &state) {
  const auto &orders = cachedOrders<Dist, PriceLevels>();
  const int nominalCancelPct = static_cast<int>(state.range(0));

  std::vector<bool> pattern(orders.size());
  std::vector<std::uint32_t> rnd(orders.size());
  {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> chance(0, 99);
    std::uniform_int_distribution<std::uint32_t> rndDist;
    for (std::size_t i = 0; i < orders.size(); ++i) {
      pattern[i] = chance(rng) < nominalCancelPct;
      rnd[i] = rndDist(rng);
    }
  }

  MapBook book;
  std::vector<std::uint64_t> activeIds;
  activeIds.reserve(orders.size());

  std::size_t idx = 0;
  std::size_t patternIdx = 0;
  std::uint64_t adds = 0, cancels = 0;

  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto &o = orders[i];
      const std::uint64_t id = book.addLimit(o.price, o.qty, o.buy);
      if (id != 0) {
        activeIds.push_back(id);
      }
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book = MapBook{};
      activeIds.clear();
      warmUp(kWarmUpCount);
      patternIdx = 0;
      state.ResumeTiming();
    }

    const bool wantCancel = pattern[patternIdx];
    ++patternIdx;
    if (patternIdx >= pattern.size()) {
      patternIdx = 0;
    }

    const bool doCancel = wantCancel && !activeIds.empty();

    if (doCancel) {
      const std::size_t pos =
          (static_cast<std::uint64_t>(rnd[patternIdx]) * activeIds.size()) >> 32;
      const std::uint64_t target = activeIds[pos];
      activeIds[pos] = activeIds.back();
      activeIds.pop_back();
      cancels += book.cancel(target) ? 1 : 0;
    } else {
      const auto &o = orders[idx++];
      benchmark::DoNotOptimize(&o);
      const std::uint64_t id = book.addLimit(o.price, o.qty, o.buy);
      if (id != 0) {
        activeIds.push_back(id);
      }
      benchmark::DoNotOptimize(&id);
      ++adds;
    }
    benchmark::ClobberMemory();
  }

  const double total = static_cast<double>(adds + cancels);
  state.counters["addPct"] = 100.0 * static_cast<double>(adds) / total;
  state.counters["cancelPct"] = 100.0 * static_cast<double>(cancels) / total;
}

// ---------------------------------------------------------------------------
// Latency of addOrder per order type. range(0) selects the type:
//   0 = Limit GTC, 1 = Market, 2 = Limit IOC, 3 = Limit FOK, 4 = Post-only.
// ---------------------------------------------------------------------------
template <std::uint32_t BookSize, std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_OrderType(benchmark::State &state) {
  using Engine = MatchingEngine<OrderBook<BookSize, PriceLevels>>;
  const auto &orders = cachedOrders<Dist, PriceLevels>();
  const int type = static_cast<int>(state.range(0));

  switch (type) {
  case 0:
    state.SetLabel("LimitGTC");
    break;
  case 1:
    state.SetLabel("Market");
    break;
  case 2:
    state.SetLabel("LimitIOC");
    break;
  case 3:
    state.SetLabel("LimitFOK");
    break;
  case 4:
    state.SetLabel("PostOnly");
    break;
  default:
    state.SetLabel("?");
    break;
  }

  auto book = std::make_unique<Engine>();

  std::size_t idx = 0;
  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      addLimit(*book, orders[i]);
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book->clear();
      warmUp(kWarmUpCount);
      state.ResumeTiming();
    }

    const auto &o = orders[idx++];

    typename Engine::Order order;
    order.quantity = o.qty;
    order.side = o.buy ? Engine::Side::Buy : Engine::Side::Sell;

    switch (type) {
    case 0:
      order.type = Engine::OrderType::Limit;
      order.tif = Engine::TimeInForce::GTC;
      order.price = static_cast<typename Engine::PriceIdx>(o.price);
      break;
    case 1:
      order.type = Engine::OrderType::Market;
      order.tif = Engine::TimeInForce::GTC;
      break;
    case 2:
      order.type = Engine::OrderType::Limit;
      order.tif = Engine::TimeInForce::IOC;
      order.price = static_cast<typename Engine::PriceIdx>(o.price);
      break;
    case 3:
      order.type = Engine::OrderType::Limit;
      order.tif = Engine::TimeInForce::FOK;
      order.price = static_cast<typename Engine::PriceIdx>(o.price);
      break;
    case 4:
      order.type = Engine::OrderType::Limit;
      order.tif = Engine::TimeInForce::GTC;
      order.postOnly = true;
      order.price = static_cast<typename Engine::PriceIdx>(o.price);
      break;
    }

    benchmark::DoNotOptimize(&order);
    const auto r = book->addOrder(order);
    benchmark::DoNotOptimize(&r);
    benchmark::ClobberMemory();
  }
}

// ---------------------------------------------------------------------------
// Baseline: the same addLimit workload against a std::map order book.
// ---------------------------------------------------------------------------
template <std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_MapLatency(benchmark::State &state) {
  const auto &orders = cachedOrders<Dist, PriceLevels>();

  MapBook book;
  std::size_t idx = 0;

  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto &o = orders[i];
      book.addLimit(o.price, o.qty, o.buy);
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book = MapBook{};
      warmUp(kWarmUpCount);
      state.ResumeTiming();
    }

    const auto &o = orders[idx++];
    benchmark::DoNotOptimize(&o);
    book.addLimit(o.price, o.qty, o.buy);
    benchmark::ClobberMemory();
  }
}

// ---------------------------------------------------------------------------
// Baseline: the same addLimit workload against an unordered_map order book.
// ---------------------------------------------------------------------------
template <std::uint32_t PriceLevels, PriceDistribution Dist>
void BM_HashMapLatency(benchmark::State &state) {
  const auto &orders = cachedOrders<Dist, PriceLevels>();

  HashMapBook book;
  std::size_t idx = 0;

  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto &o = orders[i];
      book.addLimit(o.price, o.qty, o.buy);
    }
    idx = n;
  };
  warmUp(kWarmUpCount);

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book = HashMapBook{};
      warmUp(kWarmUpCount);
      state.ResumeTiming();
    }

    const auto &o = orders[idx++];
    benchmark::DoNotOptimize(&o);
    book.addLimit(o.price, o.qty, o.buy);
    benchmark::ClobberMemory();
  }
}

// ---------------------------------------------------------------------------
// Registration.
//
// Size sweep (BookSize / PriceLevels):
//   small  = 32,768 / 1,024        (cache-resident)
//   medium = 131,072 / 16,384      (16x price range)
//   large  = 524,288 / 262,144     (256x price range)
// ---------------------------------------------------------------------------

#define FLATBOOK_REGISTER_LATENCY(Size, Levels)                                                    \
  BENCHMARK_TEMPLATE(BM_Latency, Size, Levels, PriceDistribution::Uniform);                        \
  BENCHMARK_TEMPLATE(BM_Latency, Size, Levels, PriceDistribution::Clustered)

FLATBOOK_REGISTER_LATENCY(32768, 1024);
FLATBOOK_REGISTER_LATENCY(131072, 16384);
FLATBOOK_REGISTER_LATENCY(524288, 262144);

#define FLATBOOK_REGISTER_CANCEL(Size, Levels)                                                     \
  BENCHMARK_TEMPLATE(BM_Cancel, Size, Levels, PriceDistribution::Uniform)                          \
      ->Arg(0)                                                                                     \
      ->Arg(10)                                                                                    \
      ->Arg(25)                                                                                    \
      ->Arg(50);                                                                                   \
  BENCHMARK_TEMPLATE(BM_Cancel, Size, Levels, PriceDistribution::Clustered)                        \
      ->Arg(0)                                                                                     \
      ->Arg(10)                                                                                    \
      ->Arg(25)                                                                                    \
      ->Arg(50)

FLATBOOK_REGISTER_CANCEL(32768, 1024);
FLATBOOK_REGISTER_CANCEL(131072, 16384);
FLATBOOK_REGISTER_CANCEL(524288, 262144);

#define FLATBOOK_REGISTER_ORDER_TYPE(Size, Levels)                                                 \
  BENCHMARK_TEMPLATE(BM_OrderType, Size, Levels, PriceDistribution::Uniform)                       \
      ->ArgName("orderType")                                                                       \
      ->Arg(0)                                                                                     \
      ->Arg(1)                                                                                     \
      ->Arg(2)                                                                                     \
      ->Arg(3)                                                                                     \
      ->Arg(4);                                                                                    \
  BENCHMARK_TEMPLATE(BM_OrderType, Size, Levels, PriceDistribution::Clustered)                     \
      ->ArgName("orderType")                                                                       \
      ->Arg(0)                                                                                     \
      ->Arg(1)                                                                                     \
      ->Arg(2)                                                                                     \
      ->Arg(3)                                                                                     \
      ->Arg(4)

FLATBOOK_REGISTER_ORDER_TYPE(32768, 1024);

#define FLATBOOK_REGISTER_MAP(Levels)                                                              \
  BENCHMARK_TEMPLATE(BM_MapLatency, Levels, PriceDistribution::Uniform);                           \
  BENCHMARK_TEMPLATE(BM_MapLatency, Levels, PriceDistribution::Clustered)

FLATBOOK_REGISTER_MAP(1024);
FLATBOOK_REGISTER_MAP(16384);
FLATBOOK_REGISTER_MAP(262144);

#define FLATBOOK_REGISTER_HASHMAP(Levels)                                                          \
  BENCHMARK_TEMPLATE(BM_HashMapLatency, Levels, PriceDistribution::Uniform);                       \
  BENCHMARK_TEMPLATE(BM_HashMapLatency, Levels, PriceDistribution::Clustered)

FLATBOOK_REGISTER_HASHMAP(1024);
FLATBOOK_REGISTER_HASHMAP(16384);
FLATBOOK_REGISTER_HASHMAP(262144);

#define FLATBOOK_REGISTER_MAP_CANCEL(Levels)                                                       \
  BENCHMARK_TEMPLATE(BM_MapCancel, Levels, PriceDistribution::Uniform)->Arg(0)->Arg(25)->Arg(50);  \
  BENCHMARK_TEMPLATE(BM_MapCancel, Levels, PriceDistribution::Clustered)->Arg(0)->Arg(25)->Arg(50)

FLATBOOK_REGISTER_MAP_CANCEL(1024);
FLATBOOK_REGISTER_MAP_CANCEL(16384);
FLATBOOK_REGISTER_MAP_CANCEL(262144);

// Resting-only: large FlatBook capacity so every order rests (no rejection).
#define FLATBOOK_REGISTER_REST(Levels)                                                             \
  BENCHMARK_TEMPLATE(BM_RestLatency, 262144, Levels, PriceDistribution::Uniform);                  \
  BENCHMARK_TEMPLATE(BM_RestLatency, 262144, Levels, PriceDistribution::Clustered);                \
  BENCHMARK_TEMPLATE(BM_MapRestLatency, Levels, PriceDistribution::Uniform);                       \
  BENCHMARK_TEMPLATE(BM_MapRestLatency, Levels, PriceDistribution::Clustered)

FLATBOOK_REGISTER_REST(1024);
FLATBOOK_REGISTER_REST(16384);
FLATBOOK_REGISTER_REST(262144);

#undef FLATBOOK_REGISTER_LATENCY
#undef FLATBOOK_REGISTER_CANCEL
#undef FLATBOOK_REGISTER_ORDER_TYPE
#undef FLATBOOK_REGISTER_MAP
#undef FLATBOOK_REGISTER_MAP_CANCEL
#undef FLATBOOK_REGISTER_REST
#undef FLATBOOK_REGISTER_HASHMAP

} // namespace flatbook

BENCHMARK_MAIN();
