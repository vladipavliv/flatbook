#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "flatbook/SpscQueue.hpp"

namespace flatbook {
namespace {

TEST(SpscQueue, SingleValueRoundtrip) {
  SpscQueue<4> q;
  EXPECT_TRUE(q.empty());

  const std::uint64_t sent = 0xDEADBEEFCAFEBABEull;
  ASSERT_TRUE(q.write(sent));

  std::uint64_t got = 0;
  ASSERT_TRUE(q.read(got));
  EXPECT_EQ(got, sent);
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, FifoOrder) {
  SpscQueue<8> q; // 512-byte buffer, holds 128 ints
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(q.write(i));
  }
  for (int i = 0; i < 100; ++i) {
    int v = -1;
    ASSERT_TRUE(q.read(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, FullQueueRejectsWrite) {
  SpscQueue<1> q; // 64-byte buffer = exactly 16 ints
  int v = 0;
  for (int i = 0; i < 16; ++i) {
    ASSERT_TRUE(q.write(v++));
  }
  EXPECT_FALSE(q.write(v)); // full

  int out = -1;
  for (int i = 0; i < 16; ++i) {
    ASSERT_TRUE(q.read(out));
  }
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, BatchReadWrite) {
  SpscQueue<8> q;
  constexpr int kBatch = 50;
  int src[kBatch];
  int dst[kBatch] = {};

  for (int i = 0; i < kBatch; ++i) {
    src[i] = i * 7;
  }

  EXPECT_EQ(q.writeBatch(src, kBatch), kBatch);
  EXPECT_EQ(q.readBatch(dst, kBatch), kBatch);
  for (int i = 0; i < kBatch; ++i) {
    EXPECT_EQ(dst[i], src[i]);
  }
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, WrapAround) {
  SpscQueue<1> q; // 64-byte buffer; drain fully each round so head is published
  int value = 0;
  int expected = 0;
  for (int round = 0; round < 100; ++round) {
    for (int i = 0; i < 12; ++i) {
      ASSERT_TRUE(q.write(value++));
    }
    int out = -1;
    while (q.read(out)) { // read until empty (publishes head_, reclaiming the ring)
      EXPECT_EQ(out, expected++);
    }
  }
  EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, ThreadedProducerConsumer) {
  constexpr int kCount = 1'000'000;
  SpscQueue<1 << 8> q; // 16 KB buffer

  std::vector<int> received(kCount, -1);
  int receivedCount = 0;

  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      while (!q.write(i)) {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&] {
    int v = -1;
    while (receivedCount < kCount) {
      if (q.read(v)) {
        received[receivedCount++] = v;
      }
    }
  });

  producer.join();
  consumer.join();

  for (int i = 0; i < kCount; ++i) {
    ASSERT_EQ(received[i], i) << "lost/duplicated/reordered at index " << i;
  }
}

TEST(SpscQueue, BatchWrapAround) {
  SpscQueue<1> q; // 64-byte buffer, so batches cycle the ring many times
  int value = 0;
  int expected = 0;
  for (int round = 0; round < 200; ++round) {
    int src[6]; // 24 bytes, not a multiple of 64
    for (int i = 0; i < 6; ++i) {
      src[i] = value++;
    }

    size_t written = 0;
    while (written < 6) {
      written += q.writeBatch(src + written, 6 - written);
    }

    int dst[6] = {};
    size_t read = 0;
    while (read < 6) {
      read += q.readBatch(dst + read, 6 - read);
    }

    for (int i = 0; i < 6; ++i) {
      EXPECT_EQ(dst[i], expected++);
    }
  }
  EXPECT_TRUE(q.empty());
}

} // namespace
} // namespace flatbook
