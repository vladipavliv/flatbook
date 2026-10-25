// Tail-latency harness: per-operation latency percentiles (p50..p99.9/max) for
// the core workloads, complementing the mean/median averages in flatbook_bench.
//
// Times each operation with serialized TSC (rdtscp + lfence) and subtracts the
// empty-measurement baseline, so the ~10ns p50 numbers are not dominated by the
// timer itself. Pins to a core and warns when the CPU governor is not
// 'performance' or turbo is enabled. x86 only (rdtscp).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

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
std::uint64_t baselineCycles = 0;

// Serialized TSC read: rdtscp waits for prior instructions, lfence fences the
// boundary so the measured region is not reordered around the timestamps.
inline std::uint64_t tscBegin() noexcept {
  unsigned aux = 0;
  _mm_lfence();
  return __rdtscp(&aux);
}

inline std::uint64_t tscEnd() noexcept {
  unsigned aux = 0;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

/// Measure the fixed cost of an empty tscBegin()/tscEnd() pair (subtracted from
/// every sample so tiny numbers are not swamped by the timer overhead).
std::uint64_t calibrateBaseline() {
  std::uint64_t best = ~0ull;
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t t0 = tscBegin();
    asm volatile("" ::: "memory"); // keep the empty region from being removed
    const std::uint64_t t1 = tscEnd();
    const std::uint64_t d = t1 - t0;
    if (d < best) {
      best = d;
    }
  }
  return best;
}

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

void pinToCore(std::uint32_t core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  (void)sched_setaffinity(0, sizeof(set), &set);
#else
  (void)core;
#endif
}

void warnIfNotPerformance() {
#if defined(__linux__)
  const auto readTrimmed = [](const char *path, std::string &out) -> bool {
    FILE *f = std::fopen(path, "r");
    if (!f) {
      return false;
    }
    char buf[64] = {};
    if (!std::fgets(buf, sizeof(buf), f)) {
      std::fclose(f);
      return false;
    }
    std::fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) {
      s.pop_back();
    }
    out = s;
    return true;
  };
  std::string gov;
  if (readTrimmed("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov) && gov != "performance") {
    std::printf("# WARNING: CPU governor is '%s' (not 'performance'); tail numbers are noisy\n",
                gov.c_str());
  }
  std::string turbo;
  if (readTrimmed("/sys/devices/system/cpu/intel_pstate/no_turbo", turbo) && turbo == "0") {
    std::printf("# WARNING: turbo boost is enabled; tail numbers are noisy\n");
  }
#else
  (void)0;
#endif
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
    const std::uint64_t raw = buf[static_cast<std::size_t>(q * double(samples - 1))];
    const std::uint64_t cycles = raw > baselineCycles ? raw - baselineCycles : 0;
    return double(cycles) * nsPerCycle;
  };
  return {pct(0.50), pct(0.90), pct(0.99), pct(0.999), pct(1.0)};
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
    const std::uint64_t t0 = tscBegin();
    step(*book, o);
    buf[i] = tscEnd() - t0;
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

int main(int argc, char **argv) {
  using namespace flatbook;

  std::uint32_t core = 0;
  if (argc >= 2) {
    core = static_cast<std::uint32_t>(std::atoi(argv[1]));
  }

  pinToCore(core);
  calibrateNsPerCycle();
  baselineCycles = calibrateBaseline();
  warnIfNotPerformance();

  std::printf("Tail latency (ns), TSC (rdtscp+lfence), %zu samples/row; FlatBook BookSize=%u\n",
              kSamples, kBookSize);
  std::printf("core=%u timer-baseline=%llu cycles (~%.1f ns), ns/cycle=%.4f\n\n", core,
              static_cast<unsigned long long>(baselineCycles),
              static_cast<double>(baselineCycles) * nsPerCycle, nsPerCycle);
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
