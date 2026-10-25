// Tail-latency harness: per-operation latency percentiles (p50..p99.9/max) for
// the core workloads, complementing the mean/median averages in flatbook_bench.
//
// Times each operation with the TSC (__rdtsc) and reports percentiles in ns.
// Numbers are indicative (no core pinning or frequency lock), same caveat as
// the averages. x86 only (rdtsc).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "BenchCommon.hpp"
#include "flatbook/MatchingEngine.hpp"
#include "reference/CppTraderBook.hpp"
#include "reference/MapBook.hpp"

namespace flatbook {

// Order capacity for FlatBook in this harness. Large enough that the book never
// fills during warmup + one batch, so every timed op is a real rest/match.
constexpr std::uint32_t kBookSize = 262144;

constexpr std::size_t kSamples = 100000;
constexpr std::size_t kBatch = 20000; // reset the book after this many timed ops

double nsPerCycle = 1.0;

void calibrateNsPerCycle() {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const std::uint64_t c0 = __rdtsc();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::uint64_t c1 = __rdtsc();
  const auto t1 = clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  nsPerCycle = ns / double(c1 - c0);
}

const char *distName(PriceDistribution d) {
  return d == PriceDistribution::Uniform ? "Uniform" : "Clustered";
}

struct Stats {
  double p50, p90, p99, p999, max;
};

Stats toStats(std::vector<std::uint64_t> &buf, std::size_t samples) {
  std::sort(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(samples));
  const auto pct = [&](double q) {
    return double(buf[static_cast<std::size_t>(q * double(samples - 1))]) * nsPerCycle;
  };
  return {pct(0.50), pct(0.90), pct(0.99), pct(0.999), double(buf[samples - 1]) * nsPerCycle};
}

template <typename Book, typename Step>
Stats measureTail(const std::vector<GenOrder> &orders, Step &&step, std::vector<std::uint64_t> &buf) {
  auto book = std::make_unique<Book>();
  std::size_t idx = 0;

  auto warm = [&](std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (idx >= orders.size()) {
        idx = 0;
      }
      step(*book, orders[idx++]);
    }
  };

  warm(kWarmUpCount);
  for (std::size_t i = 0; i < kSamples; ++i) {
    if (i != 0 && i % kBatch == 0) {
      book = std::make_unique<Book>();
      warm(kWarmUpCount);
    }
    if (idx >= orders.size()) {
      idx = 0;
    }
    const auto &o = orders[idx++];
    const std::uint64_t t0 = __rdtsc();
    step(*book, o);
    buf[i] = __rdtsc() - t0;
  }
  return toStats(buf, kSamples);
}

void printHeader() {
  std::printf("%-46s %9s %9s %9s %10s %10s\n", "workload", "p50", "p90", "p99", "p99.9", "max");
}

void printRow(const std::string &label, const Stats &s) {
  std::printf("%-46s %9.1f %9.1f %9.1f %10.1f %10.1f\n", label.c_str(), s.p50, s.p90, s.p99,
              s.p999, s.max);
}

template <PriceDistribution Dist, std::uint32_t PriceLevels>
void runAll(std::vector<std::uint64_t> &buf) {
  const auto &orders = cachedOrders<Dist, PriceLevels>();
  using Engine = MatchingEngine<OrderBook<kBookSize, PriceLevels>>;

  std::string base = std::to_string(PriceLevels) + " " + distName(Dist);

  printRow("add   FlatBook   " + base,
           measureTail<Engine>(orders, [](auto &e, const GenOrder &o) { (void)addLimit(e, o); }, buf));
  printRow("rest  FlatBook   " + base,
           measureTail<Engine>(orders, [](auto &e, const GenOrder &o) { (void)addRest(e, o); }, buf));
  printRow("rest  std::map   " + base,
           measureTail<MapBook>(orders, [](auto &b, const GenOrder &o) { (void)b.addLimit(o.price, o.qty, true); }, buf));
  printRow("rest  CppTrader  " + base,
           measureTail<CppTraderBook>(orders, [](auto &b, const GenOrder &o) { (void)b.addLimit(o.price, o.qty, true); }, buf));
}

} // namespace flatbook

int main() {
  using namespace flatbook;
  calibrateNsPerCycle();
  std::printf("Tail latency (ns), TSC-based, %zu samples/row; FlatBook BookSize=%u\n\n", kSamples,
              kBookSize);
  printHeader();

  std::vector<std::uint64_t> buf(kSamples);
  runAll<PriceDistribution::Uniform, 1024>(buf);
  runAll<PriceDistribution::Clustered, 1024>(buf);
  runAll<PriceDistribution::Uniform, 16384>(buf);
  runAll<PriceDistribution::Clustered, 16384>(buf);
  runAll<PriceDistribution::Uniform, 262144>(buf);
  runAll<PriceDistribution::Clustered, 262144>(buf);
  return 0;
}
