#pragma once

#include <cstdint>

#include "cpptrader/MarketManagerOptimized.hpp"

namespace flatbook {

// Adapter driving CppTrader's optimized market manager as a single-symbol
// resting book.
//
// NOTE: CppTrader's MarketManagerOptimized is a market-data replay container,
// not a matching engine - it maintains price levels but does NOT compute
// crossings. So this adapter (and BM_CppTraderRestLatency) measures the pure
// container rest/delete path, compared against BM_RestLatency (FlatBook) and
// BM_MapRestLatency (std::map).
struct CppTraderBook {
  CppTrader::Matching::MarketHandler handler_;
  CppTrader::Matching::MarketManagerOptimized market_;
  std::uint64_t nextId_{1};

  CppTraderBook() : market_(handler_, 1 << 20) {
    const CppTrader::Matching::Symbol symbol{}; // Id = 0, name zeroed
    market_.AddSymbol(symbol);
    market_.AddOrderBook(symbol);
  }

  // Rest an order (never crosses). Returns the order id.
  std::uint64_t addLimit(std::uint32_t price, std::uint32_t qty, bool buy) {
    const std::uint64_t id = nextId_++;
    market_.AddOrder(id, 0,
                     buy ? CppTrader::Matching::OrderSide::BUY : CppTrader::Matching::OrderSide::SELL,
                     price, qty);
    return id;
  }

  bool cancel(std::uint64_t id) {
    market_.DeleteOrder(id);
    return true;
  }
};

} // namespace flatbook
