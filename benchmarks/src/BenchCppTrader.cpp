#include <benchmark/benchmark.h>

#include <memory>

#include "BenchCommon.hpp"
#include "reference/CppTraderBook.hpp"

namespace flatbook {

// ---------------------------------------------------------------------------
// CppTrader optimized market manager: resting-only add (all buys). This is the
// same pure-container workload as BM_RestLatency (FlatBook) and
// BM_MapRestLatency (std::map) - it exercises price-level insertion and
// best-price maintenance, with no crossing (CppTrader's market manager is a
// market-data replay container, not a matching engine).
// ---------------------------------------------------------------------------
template <std::uint32_t PriceLevels, std::uint32_t BookSize, PriceDistribution Dist>
void BM_CppTraderRestLatency(benchmark::State &state) {
  const auto &orders = cachedOrders<Dist, PriceLevels, BookSize>();

  auto book = std::make_unique<CppTraderBook>();
  std::size_t idx = 0;

  auto warmUp = [&](std::size_t n) {
    n = std::min(n, orders.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto &o = orders[i];
      book->addLimit(o.price, o.qty, true); // force buy: always rests
    }
    idx = n;
  };
  warmUp(warmupDepth<BookSize>());

  for (auto _ : state) {
    if (idx >= orders.size()) {
      state.PauseTiming();
      book = std::make_unique<CppTraderBook>();
      warmUp(warmupDepth<BookSize>());
      state.ResumeTiming();
    }

    const auto &o = orders[idx++];
    benchmark::DoNotOptimize(&o);
    book->addLimit(o.price, o.qty, true);
    benchmark::ClobberMemory();
  }
}

#define FLATBOOK_REGISTER_CPPTRADER_REST(Size, Levels)                                             \
  BENCHMARK_TEMPLATE(BM_CppTraderRestLatency, Levels, Size, PriceDistribution::Uniform);           \
  BENCHMARK_TEMPLATE(BM_CppTraderRestLatency, Levels, Size, PriceDistribution::Clustered)

FLATBOOK_REGISTER_CPPTRADER_REST(32768, 1024);
FLATBOOK_REGISTER_CPPTRADER_REST(131072, 16384);
FLATBOOK_REGISTER_CPPTRADER_REST(524288, 262144);

#undef FLATBOOK_REGISTER_CPPTRADER_REST

} // namespace flatbook
