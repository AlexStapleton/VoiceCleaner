#include "logger.h"
#include <windows.h>
#include <cstdio>
#include <chrono>
#include <ctime>
namespace vc {

Logger::Logger(std::string csvPath, const SpscRing<Frame>* ringA, const SpscRing<Frame>* ringB, const DropoutCounter* dc,
               const std::atomic<double>* ratioPpm, const std::atomic<double>* filtOcc)
  : csvPath_(std::move(csvPath)), ringA_(ringA), ringB_(ringB), dc_(dc), ratioPpm_(ratioPpm), filtOcc_(filtOcc) {}

Logger::~Logger() { stop(); }

bool Logger::start() {
  // Probe that the CSV path is writable; run() reopens it on the telemetry thread.
  std::FILE* probe = std::fopen(csvPath_.c_str(), "w");
  if (!probe) {
    std::fprintf(stderr, "error: could not open CSV for writing: %s\n", csvPath_.c_str());
    return false;
  }
  std::fclose(probe);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]{ run(); });
  return true;
}

void Logger::run() {
  std::FILE* f = std::fopen(csvPath_.c_str(), "w");
  if (!f) return;

  // Wall-clock start for the header comment.
  auto wall = std::chrono::system_clock::now();
  std::time_t wt = std::chrono::system_clock::to_time_t(wall);
  char wallStr[64] = {0};
  std::tm tmv{};
  localtime_s(&tmv, &wt);
  std::strftime(wallStr, sizeof(wallStr), "%Y-%m-%d %H:%M:%S", &tmv);

  std::fprintf(f, "# started %s\n", wallStr);
  std::fprintf(f, "elapsed_ms,ringA_frames,ringB_frames,ringA_hi,ringB_hi,underruns,overruns,ratio_ppm,filt_occ\n");
  std::fflush(f);

  LARGE_INTEGER freq, t0;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  long long nextWriteMs = 0;
  while (running_.load(std::memory_order_acquire)) {
    size_t a = ringA_->size();
    size_t b = ringB_->size();
    occA_.sample(a);
    occB_.sample(b);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long elapsed_ms = (now.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart;

    if (elapsed_ms >= nextWriteMs) {  // true ~1 Hz, driven by real elapsed time
      uint64_t un = dc_->underruns();
      uint64_t ov = dc_->overruns();
      double ratio_ppm = ratioPpm_->load(std::memory_order_relaxed);
      double filt_occ  = filtOcc_->load(std::memory_order_relaxed);
      std::fprintf(f, "%lld,%zu,%zu,%zu,%zu,%llu,%llu,%.2f,%.1f\n",
                   elapsed_ms, a, b, occA_.high_water(), occB_.high_water(),
                   (unsigned long long)un, (unsigned long long)ov, ratio_ppm, filt_occ);
      std::fflush(f);
      std::printf("[%4llds] A=%-4zu B=%-4zu (hi A=%zu B=%zu) under=%llu over=%llu ratio=%+.1fppm filt=%.1f\n",
                  elapsed_ms / 1000, a, b, occA_.high_water(), occB_.high_water(),
                  (unsigned long long)un, (unsigned long long)ov, ratio_ppm, filt_occ);
      std::fflush(stdout);

      do { nextWriteMs += 1000; } while (nextWriteMs <= elapsed_ms);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::fclose(f);
}

void Logger::stop() {
  running_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
}

}
