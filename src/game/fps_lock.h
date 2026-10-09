#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sade {

// Keeps the game's frame-rate cap at a chosen value. The game resets its own cap (to 60) on every
// launch and does not persist the menu choice, so the mod writes the value into the objects that
// hold it. Those objects were located with the developer value scanner on SanAndreas.exe
// 1.0.113.21181; they are found again at runtime by their vtable, so this only runs on that build.
class FpsLock {
 public:
  using LogFn = std::function<void(const std::string&)>;

  ~FpsLock();

  void start(std::uintptr_t image_base, std::uintptr_t image_end, LogFn log);
  void stop();

  // 0 = leave the game's own setting alone, -1 = unlimited.
  void set_target(std::int32_t fps);
  std::int32_t target() const { return target_.load(std::memory_order_relaxed); }

  // Number of live objects found and the game's current value (-1 if unknown).
  std::int32_t objects() const { return objects_.load(std::memory_order_relaxed); }
  std::int32_t game_value() const { return game_value_.load(std::memory_order_relaxed); }

  struct Field {
    std::uint32_t vtable_rva = 0;
    std::uint32_t offset = 0;
    bool is_float = false;
  };

 private:
  struct Target {
    std::uintptr_t object = 0;
    Field field;
  };

  void loop();
  void rescan();
  bool still_valid(const Target& target) const;
  bool matches_signature(std::uintptr_t object, const Field& field) const;

  std::uintptr_t image_base_ = 0;
  std::uintptr_t image_end_ = 0;
  LogFn log_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<std::int32_t> target_{0};
  std::atomic<std::int32_t> objects_{0};
  std::atomic<std::int32_t> game_value_{-1};
  std::mutex mutex_;
  std::vector<Target> targets_;
  std::string last_summary_;
  double last_scan_ms_ = 0.0;
};

}  // namespace sade
