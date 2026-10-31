#pragma once

#include <cstdint>
#include <vector>

#include "Generator.hpp"
#include "flatbook/MatchingEngine.hpp"

namespace flatbook {

// Fraction of the book capacity pre-loaded before timing, so depth scales with
// capacity: the "latency vs capacity" claim is tested against a meaningfully
// deep book, not a small fixed warm working set.
inline constexpr double kDepthFraction = 0.5;

// Orders to pre-load into the book before timing starts.
template <std::uint32_t BookSize>
constexpr std::size_t warmupDepth() noexcept {
  return static_cast<std::size_t>(static_cast<double>(BookSize) * kDepthFraction);
}

// Total generated orders per config: the warm-up depth plus a churn tail.
template <std::uint32_t BookSize>
constexpr std::size_t orderCount() noexcept {
  return warmupDepth<BookSize>() * 2;
}

// Per (distribution, priceLevels, bookSize) order cache: generated once,
// reused by every benchmark that shares the config.
template <PriceDistribution Dist, std::uint32_t PriceLevels, std::uint32_t BookSize>
auto cachedOrders() -> const std::vector<GenOrder> & {
  static const std::vector<GenOrder> cache = [] {
    OrderGenerator gen(orderCount<BookSize>(), Dist, PriceLevels);
    return std::vector<GenOrder>(gen.orders());
  }();
  return cache;
}

template <typename Engine>
typename Engine::AddResult addLimit(Engine &book, const GenOrder &o) {
  return book.addOrder({.type = Engine::OrderType::Limit,
                        .tif = Engine::TimeInForce::GTC,
                        .price = static_cast<typename Engine::Price>(o.price),
                        .quantity = o.qty,
                        .side = o.buy ? Engine::Side::Buy : Engine::Side::Sell});
}

// Add an order that always rests (all buys: never crosses, so every insert
// allocates in the map baseline). Exercises the pure "rest" path.
template <typename Engine>
typename Engine::AddResult addRest(Engine &book, const GenOrder &o) {
  return book.addOrder({.type = Engine::OrderType::Limit,
                        .tif = Engine::TimeInForce::GTC,
                        .price = static_cast<typename Engine::Price>(o.price),
                        .quantity = o.qty,
                        .side = Engine::Side::Buy});
}

} // namespace flatbook
