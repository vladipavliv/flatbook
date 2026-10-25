#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

// MSVC needs the vendor spelling for [[no_unique_address]] to get zero-size
// empty members; GCC/Clang use the standard attribute.
#if defined(_MSC_VER) && !defined(__clang__)
#define FLATBOOK_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define FLATBOOK_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace flatbook {

/// Order quantity type (per-order).
using Quantity = uint32_t;

/// Aggregated volume type (sum of quantities across a level).
using Volume = uint64_t;

/**
 * @brief Smallest integer type that can index @p Size distinct slots.
 *
 * Resolves to uint16_t when Size fits in 16 bits, uint32_t otherwise, so small
 * books don't pay for wide indices they don't need.
 *
 * @tparam Size Maximum index count.
 */
template <std::uint32_t Size>
using IndexType = std::conditional_t<Size <= std::numeric_limits<std::uint16_t>::max(),
                                     std::uint16_t, std::uint32_t>;
} // namespace flatbook
