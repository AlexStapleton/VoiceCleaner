#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "spsc_ring.h"
using vc::SpscRing;

TEST(SpscRing, PushPopFifo) {
  SpscRing<int> r(4);
  EXPECT_EQ(r.size(), 0u);
  EXPECT_TRUE(r.try_push(1));
  EXPECT_TRUE(r.try_push(2));
  EXPECT_EQ(r.size(), 2u);
  int v = 0;
  EXPECT_TRUE(r.try_pop(v)); EXPECT_EQ(v, 1);
  EXPECT_TRUE(r.try_pop(v)); EXPECT_EQ(v, 2);
  EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, RejectsWhenFull) {
  SpscRing<int> r(2);            // capacity 2
  EXPECT_TRUE(r.try_push(10));
  EXPECT_TRUE(r.try_push(20));
  EXPECT_FALSE(r.try_push(30));  // full
  EXPECT_EQ(r.size(), 2u);
}

TEST(SpscRing, WrapsAround) {
  SpscRing<int> r(2);
  int v;
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(r.try_push(i));
    EXPECT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_EQ(r.size(), 0u);
}

TEST(SpscRing, ConcurrentProducerConsumer) {
  SpscRing<int> r(64);
  constexpr int N = 200000;
  std::thread prod([&]{
    for (int i = 0; i < N; ) { if (r.try_push(i)) ++i; }
  });
  std::vector<int> got; got.reserve(N);
  int v;
  while ((int)got.size() < N) { if (r.try_pop(v)) got.push_back(v); }
  prod.join();
  for (int i = 0; i < N; ++i) EXPECT_EQ(got[i], i);  // FIFO, no loss, no dup
}
