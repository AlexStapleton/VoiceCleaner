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

TEST(SpscRing, ClearEmptiesAndStaysUsableAfterWrap) {
  SpscRing<int> r(3);
  int v = 0;
  for (int i = 0; i < 7; ++i) { ASSERT_TRUE(r.try_push(i)); ASSERT_TRUE(r.try_pop(v)); }  // move indices off 0
  ASSERT_TRUE(r.try_push(100)); ASSERT_TRUE(r.try_push(101));
  r.clear();
  EXPECT_EQ(r.size(), 0u);
  EXPECT_FALSE(r.try_pop(v));
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(r.try_push(i));   // full capacity available again
  EXPECT_FALSE(r.try_push(3));
  for (int i = 0; i < 3; ++i) { ASSERT_TRUE(r.try_pop(v)); EXPECT_EQ(v, i); }   // FIFO order intact
}

// Device-reselect scenario (Bridge::stop -> start): a ring left at its half-full drift setpoint by the
// previous session must come back primed to exactly capacity/2, not capacity/2 on top of the residue.
TEST(SpscRing, ClearThenPrimeRestoresHalfFullAfterReselect) {
  const size_t cap = 8;
  SpscRing<int> ringB(cap);
  for (size_t i = 0; i < cap / 2; ++i) ASSERT_TRUE(ringB.try_push(-1));   // previous session's residue
  ringB.clear();
  for (size_t i = 0; i < ringB.capacity() / 2; ++i) ASSERT_TRUE(ringB.try_push(0));   // start() priming
  EXPECT_EQ(ringB.size(), cap / 2);
  int v = 1;
  while (ringB.try_pop(v)) EXPECT_EQ(v, 0);   // only fresh silence, no stale frames
}
