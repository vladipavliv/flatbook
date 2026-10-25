#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <set>

#include "flatbook/Bitmap.hpp"

namespace flatbook {
namespace {

// Fuzz the bitmap against a std::set oracle. Sizes straddle the level
// boundaries: 4096/4097 switch 1->2 levels, 262144/262145 switch 2->3 levels.
template <std::uint32_t N>
void fuzzAgainstSet() {
  using Index = typename Bitmap<N>::Index;
  constexpr Index INV = Bitmap<N>::INVALID;

  Bitmap<N> bm;
  std::set<std::uint32_t> ref;
  std::mt19937 rng(N + 1);

  for (int i = 0; i < 50000; ++i) {
    const std::uint32_t x = static_cast<std::uint32_t>(rng() % N);
    // Keep the set sparse, otherwise empty-word skipping is never exercised.
    if (ref.size() < 16 && (rng() & 1)) {
      bm.set(static_cast<Index>(x));
      ref.insert(x);
    } else {
      bm.reset(static_cast<Index>(x));
      ref.erase(x);
    }

    const std::uint32_t q = static_cast<std::uint32_t>(rng() % N);

    const auto lo = ref.lower_bound(q);
    EXPECT_EQ(bm.findFirst(static_cast<Index>(q)), lo == ref.end() ? INV : static_cast<Index>(*lo));

    const auto up = ref.upper_bound(q);
    EXPECT_EQ(bm.findLast(static_cast<Index>(q)), up == ref.begin() ? INV : static_cast<Index>(*std::prev(up)));

    EXPECT_EQ(bm.min(), ref.empty() ? INV : static_cast<Index>(*ref.begin()));
    EXPECT_EQ(bm.max(), ref.empty() ? INV : static_cast<Index>(*ref.rbegin()));

    // findFirst/findLast at/ past the end must return INVALID (the fixed OOB).
    EXPECT_EQ(bm.findFirst(static_cast<Index>(N)), INV);
    EXPECT_EQ(bm.findLast(static_cast<Index>(N)), INV);
  }
}

TEST(Bitmap, Fuzz1) { fuzzAgainstSet<1>(); }
TEST(Bitmap, Fuzz63) { fuzzAgainstSet<63>(); }
TEST(Bitmap, Fuzz64) { fuzzAgainstSet<64>(); }
TEST(Bitmap, Fuzz65) { fuzzAgainstSet<65>(); }
TEST(Bitmap, Fuzz100) { fuzzAgainstSet<100>(); }
TEST(Bitmap, Fuzz130) { fuzzAgainstSet<130>(); }
TEST(Bitmap, Fuzz4096) { fuzzAgainstSet<4096>(); }
TEST(Bitmap, Fuzz4097) { fuzzAgainstSet<4097>(); }
TEST(Bitmap, Fuzz262144) { fuzzAgainstSet<262144>(); }
TEST(Bitmap, Fuzz262145) { fuzzAgainstSet<262145>(); }

} // namespace
} // namespace flatbook
