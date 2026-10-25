#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>

#include "flatbook/Id.hpp"
#include "flatbook/IdPool.hpp"
#include "flatbook/Types.hpp"

namespace flatbook {
namespace {

// ---------------------------------------------------------------------------
// RawId (no generation)
// ---------------------------------------------------------------------------

TEST(RawId, DefaultsToIndexZero) {
  const RawId<1024> id;
  EXPECT_EQ(id.index(), 0u);
  EXPECT_EQ(id.generation(), 0u);
  EXPECT_FALSE(RawId<1024>::kHasGeneration);
}

TEST(RawId, FromIndexRoundTrips) {
  EXPECT_EQ(RawId<1024>::fromIndex(42).index(), 42u);
  EXPECT_EQ(RawId<1024>::fromIndex(0).index(), 0u);
  EXPECT_EQ(RawId<1024>::fromIndex(1023).index(), 1023u);
}

TEST(RawId, InvalidIsMaxIndex) {
  const RawId<1024> id = RawId<1024>::invalid();
  EXPECT_EQ(id.index(), std::numeric_limits<RawId<1024>::Index>::max());
  EXPECT_NE(id, RawId<1024>::fromIndex(0));
}

TEST(RawId, FromIndexAndGenIgnoresGeneration) {
  const RawId<1024> id = RawId<1024>::fromIndexAndGen(7, 99);
  EXPECT_EQ(id.index(), 7u);
  EXPECT_EQ(id.generation(), 0u);
}

TEST(RawId, BumpGenerationIsNoop) {
  RawId<1024> id = RawId<1024>::fromIndex(5);
  id.bumpGeneration();
  id.bumpGeneration();
  EXPECT_EQ(id.index(), 5u);
  EXPECT_EQ(id.generation(), 0u);
}

TEST(RawId, EqualityIsByIndex) {
  EXPECT_EQ(RawId<1024>::fromIndex(1), RawId<1024>::fromIndex(1));
  EXPECT_NE(RawId<1024>::fromIndex(1), RawId<1024>::fromIndex(2));
}

// ---------------------------------------------------------------------------
// GenerationalId (ABA protection)
// ---------------------------------------------------------------------------

TEST(GenerationalId, PacksIndexAndGeneration) {
  using Id = GenerationalId<1024, 16>;
  const Id id = Id::fromIndexAndGen(1023, 0xABCD);
  EXPECT_EQ(id.index(), 1023u);
  EXPECT_EQ(id.generation(), 0xABCDu);
}

TEST(GenerationalId, DefaultsToZero) {
  using Id = GenerationalId<1024, 16>;
  const Id id;
  EXPECT_EQ(id.index(), 0u);
  EXPECT_EQ(id.generation(), 0u);
  EXPECT_TRUE(Id::kHasGeneration);
}

TEST(GenerationalId, BumpAdvancesGeneration) {
  GenerationalId<1024, 16> id = GenerationalId<1024, 16>::fromIndex(3);
  id.bumpGeneration();
  id.bumpGeneration();
  EXPECT_EQ(id.index(), 3u);
  EXPECT_EQ(id.generation(), 2u);
}

TEST(GenerationalId, BumpWrapsAtGenBits) {
  using Id = GenerationalId<1024, 2>; // 2 generation bits => wraps after 3
  Id id = Id::fromIndexAndGen(0, 3);
  EXPECT_EQ(id.generation(), 3u);
  id.bumpGeneration();
  EXPECT_EQ(id.generation(), 0u);
  EXPECT_EQ(id.index(), 0u);
}

TEST(GenerationalId, InvalidIsAllBitsSet) {
  using Id = GenerationalId<1024, 16>;
  const Id id = Id::invalid();
  EXPECT_EQ(id.index(), std::numeric_limits<Id::Index>::max());
  EXPECT_EQ(id.generation(), (1u << 16) - 1);
}

TEST(GenerationalId, HighGenerationDoesNotCorruptIndex) {
  using Id = GenerationalId<1024, 16>;
  const Id id = Id::fromIndexAndGen(0x00FF, 0xFFFF);
  EXPECT_EQ(id.index(), 0x00FFu);
  EXPECT_EQ(id.generation(), 0xFFFFu);
}

TEST(GenerationalId, StorageWidth) {
  // index(16 bits) + gen(16 bits) fits in 32 bits; index(16) + gen(32) needs 64.
  static_assert(std::is_same_v<GenerationalId<1024, 16>::Storage, std::uint32_t>);
  static_assert(std::is_same_v<GenerationalId<1024, 32>::Storage, std::uint64_t>);
}

TEST(GenerationalId, Equality) {
  using Id = GenerationalId<1024, 16>;
  EXPECT_EQ(Id::fromIndexAndGen(1, 2), Id::fromIndexAndGen(1, 2));
  EXPECT_NE(Id::fromIndexAndGen(1, 2), Id::fromIndexAndGen(1, 3)); // gen differs
  EXPECT_NE(Id::fromIndexAndGen(1, 2), Id::fromIndexAndGen(2, 2)); // index differs
}

// ---------------------------------------------------------------------------
// IdPool
// ---------------------------------------------------------------------------

TEST(IdPool, IssuesSequentialIndicesFromOne) {
  IdPool<64, RawId<64>> pool;
  EXPECT_EQ(pool.acquire().index(), 1u);
  EXPECT_EQ(pool.acquire().index(), 2u);
  EXPECT_EQ(pool.acquire().index(), 3u);
  EXPECT_EQ(pool.active(), 3u);
}

TEST(IdPool, NeverIssuesReservedZero) {
  IdPool<64, RawId<64>> pool;
  EXPECT_NE(pool.acquire().index(), 0u);
}

TEST(IdPool, ReusesReleasedIdsLifo) {
  IdPool<64, RawId<64>> pool;
  const auto a = pool.acquire();
  const auto b = pool.acquire();
  pool.release(a);
  const auto c = pool.acquire(); // must reuse a (LIFO), not a fresh index
  EXPECT_EQ(c.index(), a.index());
  EXPECT_NE(c.index(), b.index());
  EXPECT_EQ(pool.active(), 2u); // a(now c) + b
}

TEST(IdPool, ExhaustsAtSizeMinusOne) {
  IdPool<8, RawId<8>> pool;
  for (std::uint32_t i = 0; i < 7; ++i) {
    EXPECT_NE(pool.acquire(), RawId<8>::invalid()) << "acquire " << i;
  }
  EXPECT_EQ(pool.acquire(), RawId<8>::invalid());
}

TEST(IdPool, ActiveCountTracksAcquireRelease) {
  IdPool<64, RawId<64>> pool;
  const auto a = pool.acquire();
  const auto b = pool.acquire();
  EXPECT_EQ(pool.active(), 2u);
  pool.release(a);
  EXPECT_EQ(pool.active(), 1u);
  pool.release(b);
  EXPECT_EQ(pool.active(), 0u);
}

TEST(IdPool, ClearResetsToEmpty) {
  IdPool<64, RawId<64>> pool;
  (void)pool.acquire();
  (void)pool.acquire();
  pool.clear();
  EXPECT_EQ(pool.active(), 0u);
  EXPECT_EQ(pool.acquire().index(), 1u); // fresh start
}

TEST(IdPool, BumpsGenerationOnReuse) {
  IdPool<64, GenerationalId<64, 16>> pool;
  const auto a = pool.acquire();
  pool.release(a);
  const auto b = pool.acquire();
  EXPECT_EQ(b.index(), a.index());
  EXPECT_EQ(b.generation(), a.generation() + 1);
}

} // namespace
} // namespace flatbook
