#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace flatbook {

/// Synthetic order-flow shape.
enum class PriceDistribution { Uniform, Clustered };

/// A generated order: price level index, quantity, side.
struct GenOrder {
  std::uint32_t price;
  std::uint32_t qty;
  bool buy;
};

/**
 * @brief Deterministic order-flow generator.
 *
 * Produces @p count orders whose prices fall within [kMinPrice, priceLevels)
 * for the given distribution. Uniform flow draws prices across the full
 * configured range (adversarial for best-bid/best-ask discovery); clustered
 * flow random-walks a synthetic mid price with small gaussian offsets,
 * approximating real microstructure where flow concentrates near the touch.
 */
class OrderGenerator {
public:
  OrderGenerator(std::size_t count, PriceDistribution dist, std::uint32_t priceLevels,
                 std::uint64_t seed = 42) {
    orders_.reserve(count);

    minPrice_ = 16.0;
    maxPrice_ = static_cast<double>(priceLevels) - 16.0;

    std::mt19937_64 rng(seed);
    if (dist == PriceDistribution::Uniform) {
      genUniform(count, rng);
    } else {
      genClustered(count, rng, priceLevels);
    }
  }

  [[nodiscard]]
  const std::vector<GenOrder> &orders() const noexcept {
    return orders_;
  }

private:
  void genUniform(std::size_t count, std::mt19937_64 &rng) {
    std::uniform_int_distribution<std::uint32_t> priceDist(static_cast<std::uint32_t>(minPrice_),
                                                           static_cast<std::uint32_t>(maxPrice_));
    std::uniform_int_distribution<std::uint32_t> qtyDist(1, 1000);
    std::bernoulli_distribution sideDist(0.5);

    for (std::size_t i = 0; i < count; ++i) {
      orders_.push_back({priceDist(rng), qtyDist(rng), sideDist(rng)});
    }
  }

  void genClustered(std::size_t count, std::mt19937_64 &rng, std::uint32_t priceLevels) {
    std::normal_distribution<double> offsetDist(0.0, 6.0);
    std::normal_distribution<double> walkDist(0.0, 1.5);

    std::uniform_int_distribution<std::uint32_t> qtyDist(1, 1000);
    std::bernoulli_distribution sideDist(0.5);

    double mid = priceLevels / 2.0;

    for (std::size_t i = 0; i < count; ++i) {
      mid = std::clamp(mid + walkDist(rng), minPrice_, maxPrice_);

      const double price = std::clamp(mid + offsetDist(rng), minPrice_, maxPrice_);

      orders_.push_back({static_cast<std::uint32_t>(price), qtyDist(rng), sideDist(rng)});
    }
  }

  double minPrice_{};
  double maxPrice_{};
  std::vector<GenOrder> orders_;
};

} // namespace flatbook
