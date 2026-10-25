#pragma once

#include <cstdint>
#include <vector>

#include "Generator.hpp"
#include "flatbook/MatchingEngine.hpp"

namespace flatbook {

// Number of generated orders per run. Large enough that the warm book runs
// for many iterations before it needs to be cleared and re-warmed.
inline constexpr std::uint32_t kOrderCount = 1 << 17;

// Orders to pre-load into the book before timing starts.
inline constexpr std::size_t kWarmUpCount = 20000;

// Per (distribution, priceLevels) order cache: generated once, reused by every
// benchmark that shares the config.
template <PriceDistribution Dist, std::uint32_t PriceLevels>
auto cachedOrders() -> const std::vector<GenOrder> & {
  static const std::vector<GenOrder> cache = [] {
    OrderGenerator gen(kOrderCount, Dist, PriceLevels);
    return std::vector<GenOrder>(gen.orders());
  }();
  return cache;
}

template <typename Engine>
typename Engine::AddResult addLimit(Engine &book, const GenOrder &o) {
  return book.addOrder({.type = Engine::OrderType::Limit,
                        .tif = Engine::TimeInForce::GTC,
                        .price = static_cast<typename Engine::PriceIdx>(o.price),
                        .quantity = o.qty,
                        .side = o.buy ? Engine::Side::Buy : Engine::Side::Sell});
}

// Add an order that always rests (all buys: never crosses, so every insert
// allocates in the map baseline). Exercises the pure "rest" path.
template <typename Engine>
typename Engine::AddResult addRest(Engine &book, const GenOrder &o) {
  return book.addOrder({.type = Engine::OrderType::Limit,
                        .tif = Engine::TimeInForce::GTC,
                        .price = static_cast<typename Engine::PriceIdx>(o.price),
                        .quantity = o.qty,
                        .side = Engine::Side::Buy});
}

} // namespace flatbook
