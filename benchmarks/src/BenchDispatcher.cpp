// Multi-threaded dispatcher throughput + scaling benchmark.
//
// Measures end-to-end orders/sec (ingress submit + worker matching + collector
// drain) for a fixed symbol universe, sweeping the worker count to show scaling.
// Uses a warm mixed-crossing workload, like the single-threaded benches.
//
// Thread affinity and real-time scheduling are read from a small config file
// (see benchmarks/dispatcher.conf): the ingress, collector and workers are
// pinned to dedicated cores and put on SCHED_FIFO so the numbers are not
// polluted by the OS migrating threads or by timer/softirq work on core 0.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flatbook/MatchingDispatcher.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "flatbook/OrderBook.hpp"
#include "flatbook/Threading.hpp"

namespace flatbook {

constexpr size_t kSymbols = 256;
using Book = OrderBook<1 << 12, 1 << 9, GenerationalId<1 << 12, 24>>; // 4096 orders, 512 price levels
using Engine = MatchingEngine<Book>;
using Order = Engine::Order;
using Dispatcher = MatchingDispatcher<kSymbols, Book>;

/// Thread affinity + scheduling for the benchmark (read from dispatcher.conf).
struct BenchConfig {
  std::uint32_t ingressCore = 2;      ///< core for the submitting thread
  std::uint32_t collectorCore = 3;    ///< core for the collector thread
  std::uint32_t workerFirstCore = 4;  ///< workers go on this core, +1, ...
  bool realtime = true;               ///< SCHED_FIFO on all benchmark threads
};

/// Parse a simple `key value` file (blank lines and `#` comments are ignored).
BenchConfig loadBenchConfig(const char *path) {
  BenchConfig c;
  std::FILE *f = std::fopen(path, "r");
  if (f == nullptr) {
    return c; // missing file: use defaults
  }
  char line[256];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    char key[64];
    char val[64];
    if (std::sscanf(line, " %63s %63s", key, val) != 2) {
      continue;
    }
    const std::string k(key);
    if (k == "ingress_core") {
      c.ingressCore = static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
    } else if (k == "collector_core") {
      c.collectorCore = static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
    } else if (k == "worker_first_core") {
      c.workerFirstCore = static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
    } else if (k == "realtime") {
      c.realtime = (val[0] == 't' || val[0] == 'T' || val[0] == '1');
    }
  }
  std::fclose(f);
  return c;
}

Order makeOrder(std::mt19937_64 &rng) {
  Order o;
  o.type = (rng() % 10 == 0) ? Engine::OrderType::Market : Engine::OrderType::Limit;
  o.tif = Engine::TimeInForce::GTC;
  o.price = static_cast<Engine::Price>(rng() % Book::kPriceLevels);
  o.quantity = static_cast<Quantity>(1 + rng() % 100);
  o.side = (rng() & 1) ? Engine::Side::Buy : Engine::Side::Sell;
  return o;
}

double bench(size_t workerCount, size_t orderCount, uint64_t seed, const BenchConfig &cfg) {
  std::vector<std::pair<uint32_t, Order>> orders(orderCount);
  std::mt19937_64 rng(seed);
  for (auto &[sym, o] : orders) {
    sym = static_cast<uint32_t>(rng() % kSymbols);
    o = makeOrder(rng);
  }

  MatchingConfig config;
  config.workerCores.resize(workerCount);
  for (size_t i = 0; i < workerCount; ++i) {
    config.workerCores[i] = cfg.workerFirstCore + static_cast<uint32_t>(i);
  }
  config.collectorCore = cfg.collectorCore;
  config.realTime = cfg.realtime;

  std::atomic<size_t> results{0};
  size_t localResults = 0;
  auto dispatcher = makeDispatcher<kSymbols, Book>(config, [&](const Dispatcher::Event &ev) {
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

int main(int argc, char **argv) {
  using namespace flatbook;

  const char *configPath = (argc > 1) ? argv[1] : "benchmarks/dispatcher.conf";
  const BenchConfig cfg = loadBenchConfig(configPath);

  // Pin the ingress (this thread) and put it on real-time scheduling.
  pinCurrentThread(cfg.ingressCore);
  if (cfg.realtime) {
    setCurrentThreadRealTime();
  }

  std::printf("ingress core %u, collector core %u, workers from core %u, realtime %s\n",
              cfg.ingressCore, cfg.collectorCore, cfg.workerFirstCore,
              cfg.realtime ? "on" : "off");

  constexpr size_t kOrders = 1'000'000;

  std::printf("%-8s %-8s %14s %8s\n", "workers", "symbols", "orders/sec", "speedup");
  double base = 0.0;
  for (const size_t workers : {size_t{1}, size_t{2}, size_t{4}, size_t{8}}) {
    const double ops = bench(workers, kOrders, 42, cfg);
    if (workers == 1) {
      base = ops;
    }
    const double speedup = base > 0.0 ? ops / base : 1.0;
    std::printf("%-8zu %-8zu %14.0f %7.2fx\n", workers, kSymbols, ops, speedup);
  }
  return 0;
}
