#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "Types.hpp"

namespace flatbook {

/**
 * @brief Compile-time contract for an order id type.
 *
 * An id is a thin value type wrapping a dense slot index, optionally with an
 * ABA-protection generation. It must be regular (default-constructible,
 * copyable, equality-comparable) and expose the static constructors and
 * accessors that IdPool and OrderBook rely on. Satisfied by @ref RawId and
 * @ref GenerationalId.
 *
 * @tparam T     Candidate id type.
 * @tparam Index Raw slot index type (IndexType<Size>).
 */
template <typename T, typename Index>
concept OrderIdLike = std::regular<T> && requires(T &id) {
  requires std::same_as<std::remove_cv_t<decltype(T::kHasGeneration)>, bool>;

  { T::invalid() } -> std::same_as<T>;
  { T::fromIndex(std::declval<Index>()) } -> std::same_as<T>;
  { T::fromIndexAndGen(std::declval<Index>(), std::declval<std::uint32_t>()) } -> std::same_as<T>;

  { id.index() } -> std::convertible_to<Index>;
  { id.generation() } -> std::convertible_to<std::uint32_t>;
  id.bumpGeneration();
};

/**
 * @brief Compile-time contract for the order book consumed by MatchingEngine.
 *
 * Encodes the public surface the engine drives: price-time crossing reads,
 * insertion/removal, best-price discovery and level iteration. Satisfied by
 * @ref OrderBook.
 *
 * @note `reduce()` (partial fill) is deliberately not part of this contract:
 * it is a trusted private operation granted to the engine through friendship,
 * not a public API of the book.
 *
 * @tparam B Candidate order book type.
 */
template <typename B>
concept OrderBookLike = requires {
  typename B::OrderIdx;
  typename B::PriceIdx;
  typename B::Side;
  typename B::ParticipantType;

  requires std::same_as<std::remove_cv_t<decltype(B::kHasParticipants)>, bool>;
  requires std::same_as<std::remove_cv_t<decltype(B::kPriceLevels)>, std::uint32_t>;
  requires std::same_as<std::remove_cv_t<decltype(B::INVALID_ORDER)>, typename B::OrderIdx>;
  requires std::same_as<std::remove_cv_t<decltype(B::INVALID_PRICE)>, typename B::PriceIdx>;
} && requires(B &b, const B &cb) {
  { b.insert(std::declval<typename B::PriceIdx>(), Quantity{},
             std::declval<typename B::Side>(), std::declval<typename B::ParticipantType>()) }
      -> std::same_as<typename B::OrderIdx>;
  { b.insert(std::declval<typename B::PriceIdx>(), Quantity{}, std::declval<typename B::Side>()) }
      -> std::same_as<typename B::OrderIdx>;

  { b.remove(std::declval<typename B::OrderIdx>()) } -> std::same_as<bool>;
  { cb.isLive(std::declval<typename B::OrderIdx>()) } -> std::same_as<bool>;

  { cb.sideOf(std::declval<typename B::OrderIdx>()) } -> std::same_as<typename B::Side>;
  { cb.volume(std::declval<typename B::PriceIdx>(), std::declval<typename B::Side>()) }
      -> std::same_as<Volume>;
  { cb.head(std::declval<typename B::PriceIdx>(), std::declval<typename B::Side>()) }
      -> std::same_as<typename B::OrderIdx>;
  { cb.nextOf(std::declval<typename B::OrderIdx>()) } -> std::same_as<typename B::OrderIdx>;
  { cb.qty(std::declval<typename B::OrderIdx>()) } -> std::same_as<Quantity>;
  { cb.participantOf(std::declval<typename B::OrderIdx>()) }
      -> std::same_as<typename B::ParticipantType>;

  { cb.bestBid() } -> std::same_as<typename B::PriceIdx>;
  { cb.bestAsk() } -> std::same_as<typename B::PriceIdx>;
  { cb.nextLevel(std::declval<typename B::PriceIdx>(), std::declval<typename B::Side>()) }
      -> std::same_as<typename B::PriceIdx>;
  { cb.openOrders() } -> std::same_as<std::size_t>;

  b.clear();
};

} // namespace flatbook
