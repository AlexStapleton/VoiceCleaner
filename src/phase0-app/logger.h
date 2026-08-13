#pragma once
#include <string>
#include <atomic>
#include <thread>
#include "spsc_ring.h"
#include "frame.h"
#include "occupancy.h"
#include "dropout.h"
namespace vc {
// Non-RT telemetry: samples ring occupancy at ~100 Hz to catch high-water, writes one CSV row/sec
// and prints a ~1 Hz console status line. Never touched by the RT threads.
class Logger {
public:
  Logger(std::string csvPath, const SpscRing<Frame>* ringA, const SpscRing<Frame>* ringB, const DropoutCounter* dc);
  bool start();
  void stop();
  ~Logger();
private:
  void run();
  std::string csvPath_;
  const SpscRing<Frame>* ringA_;
  const SpscRing<Frame>* ringB_;
  const DropoutCounter*  dc_;
  OccupancyStats occA_, occB_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};
}
