#pragma once

#include <cstdint>
#include <limits>

#include "Types.hpp"

namespace flatbook {

/// Empty tag for the non-generational id path: keeps the conditional
/// generation member in OrderBook::Node at zero size via [[no_unique_address]].
struct NoGeneration {};

/// Empty tag for the no-STP path: keeps the conditional participant member in
/// OrderBook::Node at zero size. Distinct from NoGeneration so two empty
/// [[no_unique_address]] members can share the same address.
struct NoParticipant {};

/**
 * @brief Plain dense order id (a thin wrapper over the raw slot index).
 *
 * This is the default id type. It carries no generation: `generation()` is a
 * compile-time-constant 0 and `bumpGeneration()` is a no-op, so the pool and
 * the book compile down to exactly the pre-generation code path with no extra
 * per-slot state.
 *
 * @tparam Size Number of slots (index width is IndexType<Size>).
 */
template <std::uint32_t Size>
class RawId {
public:
  using Index = IndexType<Size>;

  static constexpr bool kHasGeneration = false;

  constexpr RawId() noexcept = default;

  /// Implicit from the raw index: lets RawId act like the index for sentinels.
  constexpr RawId(Index value) noexcept : value_(value) {}

  /// @return The raw slot index (array subscript).
  [[nodiscard]] constexpr Index index() const noexcept { return value_; }

  /// @return Always 0 - no generation.
  [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return 0; }

  /// No-op - no generation to advance.
  constexpr void bumpGeneration() noexcept {}

  /// @return Id for a fresh slot (generation 0).
  [[nodiscard]] static constexpr RawId fromIndex(Index index) noexcept { return RawId{index}; }

  /// @return Id for a slot; generation ignored.
  [[nodiscard]] static constexpr RawId fromIndexAndGen(Index index, std::uint32_t) noexcept {
    return RawId{index};
  }

  /// @return The "no order" sentinel (max index).
  [[nodiscard]] static constexpr RawId invalid() noexcept {
    return RawId{std::numeric_limits<Index>::max()};
  }

  constexpr bool operator==(const RawId &) const noexcept = default;

private:
  Index value_{};
};

/**
 * @brief Order id carrying an ABA-protection generation.
 *
 * Packs a dense slot index (low bits) and a generation counter (high bits)
 * into the smallest integer that fits both. The generation is advanced by the
 * pool on every reuse, so a stale id (old generation) never matches a slot
 * that has since been recycled.
 *
 * @tparam Size    Number of slots (index width is IndexType<Size>).
 * @tparam GenBits Generation width in bits (<= 32). Default 32 = ~4.3B reuses
 *                 per slot before the generation wraps; 32 bits is free because
 *                 a generational id is already a uint64_t for any index width.
 */
template <std::uint32_t Size, std::uint32_t GenBits = 32>
class GenerationalId {
public:
  using Index = IndexType<Size>;

  static constexpr bool kHasGeneration = true;
  static constexpr std::uint32_t kIndexBits = 8 * sizeof(Index);
  static constexpr std::uint32_t kGenBits = GenBits;
  static constexpr std::uint32_t kTotalBits = kIndexBits + kGenBits;

  static_assert(kTotalBits <= 64, "GenerationalId: index + generation exceed 64 bits");
  static_assert(GenBits > 0 && GenBits <= 32, "GenerationalId: generation must be 1..32 bits");

  using Storage = std::conditional_t<(kTotalBits <= 32), std::uint32_t, std::uint64_t>;

  static constexpr Storage kIndexMask = (Storage{1} << kIndexBits) - 1;
  static constexpr Storage kGenMask = (Storage{1} << kGenBits) - 1;

  constexpr GenerationalId() noexcept = default;

  /// @return The raw slot index (low bits).
  [[nodiscard]] constexpr Index index() const noexcept {
    return static_cast<Index>(value_ & kIndexMask);
  }

  /// @return The generation (high bits).
  [[nodiscard]] constexpr std::uint32_t generation() const noexcept {
    return static_cast<std::uint32_t>(value_ >> kIndexBits);
  }

  /// Advance the generation (wraps silently modulo 2^GenBits).
  constexpr void bumpGeneration() noexcept {
    value_ = (value_ & kIndexMask) | ((((value_ >> kIndexBits) + 1) & kGenMask) << kIndexBits);
  }

  /// @return Id for a fresh slot (generation 0).
  [[nodiscard]] static constexpr GenerationalId fromIndex(Index index) noexcept {
    return GenerationalId{static_cast<Storage>(index)};
  }

  /// @return Id for a slot with an explicit generation.
  [[nodiscard]] static constexpr GenerationalId fromIndexAndGen(Index index,
                                                                std::uint32_t gen) noexcept {
    return GenerationalId{static_cast<Storage>(index) | (static_cast<Storage>(gen) << kIndexBits)};
  }

  /// @return The "no order" sentinel (all bits set).
  [[nodiscard]] static constexpr GenerationalId invalid() noexcept {
    return GenerationalId{~Storage{0}};
  }

  constexpr bool operator==(const GenerationalId &) const noexcept = default;

private:
  explicit constexpr GenerationalId(Storage value) noexcept : value_(value) {}

  Storage value_{};
};

} // namespace flatbook
