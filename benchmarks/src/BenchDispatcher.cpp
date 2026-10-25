// Multi-threaded dispatcher throughput + scaling benchmark.
//
// Measures end-to-end orders/sec (ingress submit + worker matching + collector
// drain) for a fixed symbol universe, sweeping the worker count to show scaling.
// Uses a warm mixed-crossing workload, like the single-threaded benches.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "flatbook/MatchingDispatcher.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"

namespace flatbook {

constexpr size_t kSymbols = 256;
using Book = OrderBook<1 << 12, 1 << 9>; // 4096 orders, 512 price levels
using Engine = MatchingEngine<Book>;
using Order = Engine::Order;
using Dispatcher = MatchingDispatcher<kSymbols, Book>;

Order makeOrder(std::mt19937_64 &rng) {
  Order o;
  o.type = (rng() % 10 == 0) ? Engine::OrderType::Market : Engine::OrderType::Limit;
  o.tif = Engine::TimeInForce::GTC;
  o.price = static_cast<Engine::Price>(rng() % Book::kPriceLevels);
  o.quantity = static_cast<Quantity>(1 + rng() % 100);
  o.side = (rng() & 1) ? Engine::Side::Buy : Engine::Side::Sell;
  return o;
}

double bench(size_t workerCount, size_t orderCount, uint64_t seed) {
  std::vector<std::pair<uint32_t, Order>> orders(orderCount);
  std::mt19937_64 rng(seed);
  for (auto &[sym, o] : orders) {
    sym = static_cast<uint32_t>(rng() % kSymbols);
    o = makeOrder(rng);
  }

  MatchingConfig config;
  config.workerCores.resize(workerCount);
  for (size_t i = 0; i < workerCount; ++i) {
    config.workerCores[i] = static_cast<uint32_t>(i);
  }

  std::atomic<size_t> results{0};
  size_t localResults = 0;
  Dispatcher dispatcher(config, [&](const Dispatcher::Event &ev) {
    if (ev.kind == Dispatcher::Event::Kind::Result) {
      ++localResults; // collector is single-threaded: plain increment
      results.store(localResults, std::memory_order_relaxed);
    }
  });

  dispatcher.start();

  // Warm up with the first 10%, then wait for those to drain.
  const size_t warmup = orderCount / 10;
  for (size_t i = 0; i < warmup; ++i) {
    dispatcher.addOrder(orders[i].first, orders[i].second);
  }
  while (results.load(std::memory_order_relaxed) < warmup) {
    std::this_thread::yield();
  }

  // Measure: submit the rest, then wait for every result to come back.
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = warmup; i < orderCount; ++i) {
    dispatcher.addOrder(orders[i].first, orders[i].second);
  }
  while (results.load(std::memory_order_relaxed) < orderCount) {
    std::this_thread::yield();
  }
  const auto t1 = std::chrono::steady_clock::now();

  dispatcher.stop();

  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(orderCount - warmup) / seconds;
}

} // namespace flatbook

int main() {
  using namespace flatbook;

  constexpr size_t kOrders = 1'000'000;

  std::printf("%-8s %-8s %14s %8s\n", "workers", "symbols", "orders/sec", "speedup");
  double base = 0.0;
  for (const size_t workers : {size_t{1}, size_t{2}, size_t{4}, size_t{8}}) {
    const double ops = bench(workers, kOrders, 42);
    if (workers == 1) {
      base = ops;
    }
    const double speedup = base > 0.0 ? ops / base : 1.0;
    std::printf("%-8zu %-8zu %14.0f %7.2fx\n", workers, kSymbols, ops, speedup);
  }
  return 0;
}
