#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace sade {

// Estimates a mouse's report rate from packet timestamps.
// The rate is 1 / median interval between consecutive reports while the mouse moves: a 1000 Hz
// mouse reports every 1 ms while it moves, even if slow movement produces fewer reports overall.
// Intervals under 0.05 ms are ignored (several reports handed over in one batch).
// Not thread-safe: callers serialize access.
class MouseRateMeter {
 public:
  struct Snapshot {
    std::int32_t polling_hz = 0;
    std::int32_t packets_last_second = 0;
    std::int32_t max_gap_us_last_second = 0;  // Longest pause between reports while moving.
  };

  explicit MouseRateMeter(std::int64_t qpc_frequency) : qpc_frequency_(qpc_frequency > 0 ? qpc_frequency : 1) {}

  // Returns true when a new one-second snapshot is ready in `out`.
  bool record(std::int64_t qpc, Snapshot& out) {
    if (previous_qpc_ != 0) {
      const std::int64_t gap = qpc - previous_qpc_;
      if (gap * 20000 >= qpc_frequency_ && gap * 1000 < qpc_frequency_ * kMovingGapMs) {
        intervals_[next_] = gap;
        next_ = (next_ + 1) % intervals_.size();
        count_ = std::min(count_ + 1, intervals_.size());
        max_gap_ticks_ = std::max(max_gap_ticks_, gap);
      }
    }
    previous_qpc_ = qpc;
    ++window_packets_;
    if (window_start_qpc_ == 0) {
      window_start_qpc_ = qpc;
    }
    if (qpc - window_start_qpc_ < qpc_frequency_) {
      return false;
    }

    out = last_;
    if (count_ >= 32) {
      std::array<std::int64_t, kSamples> sorted{};
      std::copy_n(intervals_.begin(), count_, sorted.begin());
      const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(count_ / 2);
      std::nth_element(sorted.begin(), middle, sorted.begin() + static_cast<std::ptrdiff_t>(count_));
      if (*middle > 0) {
        out.polling_hz = static_cast<std::int32_t>(static_cast<double>(qpc_frequency_) / static_cast<double>(*middle) + 0.5);
      }
    }
    out.packets_last_second = window_packets_;
    out.max_gap_us_last_second = static_cast<std::int32_t>(max_gap_ticks_ * 1000000 / qpc_frequency_);
    last_ = out;
    window_start_qpc_ = qpc;
    window_packets_ = 0;
    max_gap_ticks_ = 0;
    return true;
  }

 private:
  static constexpr std::int64_t kMovingGapMs = 50;  // Longer pauses mean the mouse stopped.
  static constexpr std::size_t kSamples = 256;

  std::int64_t qpc_frequency_ = 1;
  std::array<std::int64_t, kSamples> intervals_{};
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::int64_t previous_qpc_ = 0;
  std::int64_t window_start_qpc_ = 0;
  std::int64_t max_gap_ticks_ = 0;
  std::int32_t window_packets_ = 0;
  Snapshot last_{};
};

}  // namespace sade
