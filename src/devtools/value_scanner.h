#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sade {

// Developer tool: finds where the game keeps a setting by scanning its private writable memory
// for a value, then narrowing the candidates as the user changes the setting in the game menu.
// Each value is searched as int32, float and double at the same time.
class ValueScanner {
 public:
  enum Kind : std::size_t { Int32 = 0, Float = 1, Double = 2, KindCount = 3 };

  struct Candidate {
    std::uintptr_t address = 0;
    Kind kind = Int32;
  };

  struct Status {
    bool busy = false;
    int scans_done = 0;
    std::array<std::uint64_t, KindCount> counts{};
    std::uint64_t bytes_scanned = 0;
    double last_scan_seconds = 0.0;
  };

  ~ValueScanner();

  // first = true discards previous candidates and scans all memory.
  bool start_scan(double value, bool first);
  Status status() const;
  // Writes to every remaining candidate, only when there are at most `limit` of them in total.
  int write_all(double value, std::size_t limit);
  std::vector<Candidate> candidates(std::size_t limit) const;

 private:
  void run(double value, bool first);

  mutable std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> busy_{false};
  std::array<std::vector<std::uintptr_t>, KindCount> lists_;
  int scans_done_ = 0;
  std::uint64_t bytes_scanned_ = 0;
  double last_scan_seconds_ = 0.0;
};

// Text description of a candidate for the log: memory region and nearby pointers into the game
// executable (likely the owning object's vtable), as RVAs.
std::string describe_candidate(const ValueScanner::Candidate& candidate, std::uintptr_t image_base, std::uintptr_t image_end);

}  // namespace sade
