#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

#include "Types.hpp"

namespace flatbook {

/**
 * @brief Hierarchical (multi-level) bitmap for O(1)-ish min/max discovery.
 *
 * Level 0 holds the raw bits (ceil(Size/64) words); each higher level is a
 * 64:1 summary of the level below, until a single word remains. min()/max()
 * then walk a constant number of levels (log64(Size)), so discovery cost does
 * not grow with the configured size. findFirst()/findLast() use the summaries
 * to skip empty words instead of scanning linearly.
 *
 * `set()`, `reset()` and `contains()` require `index < Size`; this is asserted
 * and is unchecked (undefined behavior) under `NDEBUG`.
 *
 * @tparam Size Number of bits (e.g. price levels).
 */
template <uint32_t Size>
class Bitmap {
public:
  using Index = IndexType<Size>;

  /// Index returned when no bit is set.
  static constexpr Index INVALID = std::numeric_limits<Index>::max();

private:
  static constexpr uint32_t WORD_BITS = 64;

  // Number of levels: level 0 = ceil(Size/64) words, each level above is a
  // 64:1 summary, stopping once a single word remains.
  static constexpr uint32_t kNumLevels = [] {
    uint32_t w = (Size + WORD_BITS - 1) / WORD_BITS;
    uint32_t n = 1;
    while (w > 1) {
      w = (w + WORD_BITS - 1) / WORD_BITS;
      ++n;
    }
    return n;
  }();

  // Word count per level.
  static constexpr std::array<uint32_t, kNumLevels> kWords = [] {
    std::array<uint32_t, kNumLevels> w{};
    w[0] = (Size + WORD_BITS - 1) / WORD_BITS;
    for (uint32_t l = 1; l < kNumLevels; ++l) {
      w[l] = (w[l - 1] + WORD_BITS - 1) / WORD_BITS;
    }
    return w;
  }();

  // Offset of each level into storage_.
  static constexpr std::array<uint32_t, kNumLevels> kOffsets = [] {
    std::array<uint32_t, kNumLevels> o{};
    for (uint32_t l = 1; l < kNumLevels; ++l) {
      o[l] = o[l - 1] + kWords[l - 1];
    }
    return o;
  }();

  static constexpr uint32_t kTotalWords = kOffsets[kNumLevels - 1] + kWords[kNumLevels - 1];

public:
  /// Set bit @p index.
  constexpr void set(Index index) noexcept {
    assert(index < Size);
    uint32_t w = index >> 6;
    uint64_t b = 1ULL << (index & 63);
    for (uint32_t l = 0; l < kNumLevels; ++l) {
      uint64_t &word = storage_[kOffsets[l] + w];
      word |= b;
      if (word != b) {
        return; // word already had other bits; summaries are already correct
      }
      if (l + 1 == kNumLevels) {
        return;
      }
      const uint32_t oldW = w;
      w = oldW >> 6;
      b = 1ULL << (oldW & 63);
    }
  }

  /// Clear bit @p index.
  constexpr void reset(Index index) noexcept {
    assert(index < Size);
    uint32_t w = index >> 6;
    uint64_t b = 1ULL << (index & 63);
    for (uint32_t l = 0; l < kNumLevels; ++l) {
      uint64_t &word = storage_[kOffsets[l] + w];
      word &= ~b;
      if (word != 0) {
        return; // word still has bits; summaries stay
      }
      if (l + 1 == kNumLevels) {
        return;
      }
      const uint32_t oldW = w;
      w = oldW >> 6;
      b = 1ULL << (oldW & 63);
    }
  }

  /// @return true if bit @p index is set.
  [[nodiscard]]
  constexpr bool contains(Index index) const noexcept {
    assert(index < Size);
    return (storage_[index >> 6] & (1ULL << (index & 63))) != 0;
  }

  /// Clear every bit.
  constexpr void clear() noexcept {
    storage_.fill(0);
  }

  /// @return Lowest set bit, or INVALID if empty.
  [[nodiscard]]
  constexpr Index min() const noexcept {
    if (storage_[kOffsets[kNumLevels - 1]] == 0) {
      return INVALID;
    }
    uint32_t w = 0;
    for (uint32_t l = kNumLevels; l-- > 0;) {
      const uint64_t word = storage_[kOffsets[l] + w];
      const uint32_t bit = std::countr_zero(word);
      if (l == 0) {
        return static_cast<Index>(w * WORD_BITS + bit);
      }
      w = w * WORD_BITS + bit;
    }
    return INVALID;
  }

  /// @return Highest set bit, or INVALID if empty.
  [[nodiscard]]
  constexpr Index max() const noexcept {
    if (storage_[kOffsets[kNumLevels - 1]] == 0) {
      return INVALID;
    }
    uint32_t w = 0;
    for (uint32_t l = kNumLevels; l-- > 0;) {
      const uint64_t word = storage_[kOffsets[l] + w];
      const uint32_t bit = WORD_BITS - 1 - std::countl_zero(word);
      if (l == 0) {
        return static_cast<Index>(w * WORD_BITS + bit);
      }
      w = w * WORD_BITS + bit;
    }
    return INVALID;
  }

  /// @return Lowest set bit >= @p from, or INVALID if none.
  [[nodiscard]]
  constexpr Index findFirst(Index from) const noexcept {
    if (from >= Size) {
      return INVALID;
    }

    uint32_t w = from >> 6;
    const uint64_t v = storage_[w] & (~0ULL << (from & 63));
    if (v != 0) {
      return static_cast<Index>(w * WORD_BITS + std::countr_zero(v));
    }

    ++w; // next level-0 word
    for (uint32_t l = 0; l + 1 < kNumLevels; ++l) {
      const uint32_t sw = w >> 6;
      if (sw >= kWords[l + 1]) {
        return INVALID;
      }
      const uint64_t sv = storage_[kOffsets[l + 1] + sw] & (~0ULL << (w & 63));
      if (sv != 0) {
        uint32_t wl = sw * WORD_BITS + std::countr_zero(sv); // non-empty word at level l
        for (uint32_t d = l; d > 0; --d) {
          wl = wl * WORD_BITS + std::countr_zero(storage_[kOffsets[d] + wl]);
        }
        return static_cast<Index>(wl * WORD_BITS + std::countr_zero(storage_[wl]));
      }
      w = sw + 1; // move up one level
    }
    return INVALID;
  }

  /// @return Highest set bit <= @p from, or INVALID if none.
  [[nodiscard]]
  constexpr Index findLast(Index from) const noexcept {
    if (from >= Size) {
      return INVALID;
    }

    uint32_t w = from >> 6;
    const uint64_t v = storage_[w] & (~0ULL >> (WORD_BITS - 1 - (from & 63)));
    if (v != 0) {
      return static_cast<Index>(w * WORD_BITS + (WORD_BITS - 1 - std::countl_zero(v)));
    }

    if (w == 0) {
      return INVALID;
    }
    --w; // previous level-0 word
    for (uint32_t l = 0; l + 1 < kNumLevels; ++l) {
      const uint32_t sw = w >> 6;
      const uint64_t sv = storage_[kOffsets[l + 1] + sw] & (~0ULL >> (WORD_BITS - 1 - (w & 63)));
      if (sv != 0) {
        uint32_t wl = sw * WORD_BITS + (WORD_BITS - 1 - std::countl_zero(sv)); // non-empty word at level l
        for (uint32_t d = l; d > 0; --d) {
          wl = wl * WORD_BITS + (WORD_BITS - 1 - std::countl_zero(storage_[kOffsets[d] + wl]));
        }
        return static_cast<Index>(wl * WORD_BITS + (WORD_BITS - 1 - std::countl_zero(storage_[wl])));
      }
      if (sw == 0) {
        return INVALID;
      }
      w = sw - 1; // move up one level
    }
    return INVALID;
  }

private:
  std::array<uint64_t, kTotalWords> storage_{};
};

} // namespace flatbook
