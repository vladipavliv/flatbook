#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

#include "Concepts.hpp"
#include "Id.hpp"
#include "Types.hpp"

namespace flatbook {

/**
 * @brief Free-list allocator of dense order ids.
 *
 * Ids are indices into a flat slot array. Released ids go onto a LIFO stack and
 * are reused, keeping the working set of recently-touched slots hot in cache.
 * Index 0 is reserved (never issued); `invalid()` marks exhaustion.
 *
 * The id type is a template parameter. With the default `RawId` the pool is a
 * plain index free-list (no generation). With `GenerationalId` the freed id's
 * generation is preserved in the free stack and advanced on every acquire, so
 * a stale id never aliases a recycled slot. All generation state lives here -
 * the book never manages it.
 *
 * @tparam Size Maximum number of slots.
 * @tparam Id   Id type satisfying the order-id interface (RawId / GenerationalId).
 */
template <uint32_t Size, OrderIdLike<IndexType<Size>> Id = RawId<Size>>
class IdPool {
public:
  using Index = IndexType<Size>;

  static_assert(Size < Id::invalid().index(),
                "IdPool: Size must leave room for the invalid sentinel");

  /**
   * @brief Allocate the next id.
   * @return A fresh or recycled id (with advanced generation), or invalid() when full.
   */
  [[nodiscard]] Id acquire() noexcept {
    if (freeTop_ != 0) {
      Id id = freeStack_[--freeTop_];
      id.bumpGeneration();
      return id;
    }
    if (next_.index() >= Size) {
      return Id::invalid();
    }
    Id id = next_;
    next_ = Id::fromIndex(next_.index() + 1);
    return id;
  }

  /**
   * @brief Return an id to the free list.
   * @param id Id previously returned by acquire(); @pre not already released
   *           (asserted; unchecked under NDEBUG).
   */
  void release(Id id) noexcept {
    assert(id.index() != 0 && id.index() < Size);
    assert(freeTop_ < Size);
    freeStack_[freeTop_++] = id;
  }

  /// @return Number of currently allocated (live) ids.
  [[nodiscard]] size_t active() const noexcept {
    return static_cast<size_t>(next_.index()) - 1 - static_cast<size_t>(freeTop_);
  }

  /// Reset the pool to its empty state.
  ///
  /// @note This invalidates all outstanding ids: after clear(), an id issued
  /// before the call may alias a newly issued id (ABA protection does not span
  /// a clear). Do not reuse pre-clear ids across a clear.
  void clear() noexcept {
    freeTop_ = 0;
    next_ = Id::fromIndex(1);
  }

  /// Raw snapshot of the pool, for serialization / restore. Carries the full
  /// ids (generation included), so a restored pool keeps issuing the same ids.
  struct Snapshot {
    std::array<Id, Size> freeStack{};
    uint32_t freeTop{0};
    Id next{Id::fromIndex(1)};
  };

  /// @return A copy of the pool state (free list, top, next id).
  [[nodiscard]]
  Snapshot snapshot() const noexcept {
    return Snapshot{freeStack_, freeTop_, next_};
  }

  /// Restore the pool state.
  void restore(const Snapshot &s) noexcept {
    freeStack_ = s.freeStack;
    freeTop_ = s.freeTop;
    next_ = s.next;
  }

private:
  std::array<Id, Size> freeStack_{};
  uint32_t freeTop_{};
  Id next_{Id::fromIndex(1)};
};

} // namespace flatbook
