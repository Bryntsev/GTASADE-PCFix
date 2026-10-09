#include "game/fps_lock.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sade {
namespace {

constexpr std::size_t kChunkBytes = 4U * 1024U * 1024U;

bool read_memory(std::uintptr_t address, void* out, std::size_t size) {
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &read) != FALSE &&
         read == size;
}

bool write_memory(std::uintptr_t address, const void* data, std::size_t size) {
  SIZE_T written = 0;
  return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, size, &written) != FALSE &&
         written == size;
}

}  // namespace

FpsLock::~FpsLock() {
  stop();
}

void FpsLock::start(std::uintptr_t image_base, std::uintptr_t image_end, LogFn log) {
  if (worker_.joinable()) {
    return;
  }
  image_base_ = image_base;
  image_end_ = image_end;
  log_ = std::move(log);
  stop_.store(false);
  worker_ = std::thread([this] { loop(); });
}

void FpsLock::stop() {
  stop_.store(true);
  if (worker_.joinable()) {
    worker_.join();
  }
}

void FpsLock::set_target(std::int32_t fps) {
  target_.store(std::clamp(fps, -1, 1000), std::memory_order_relaxed);
}

bool FpsLock::still_valid(const Target& target) const {
  std::uint64_t vtable = 0;
  return read_memory(target.object, &vtable, sizeof(vtable)) && vtable == image_base_ + target.field.vtable_rva;
}

namespace {

// Fields found with the value scanner (see docs/FPS_LOCK.md). int32: menu value in FPS
// (0 = unlimited); floats: frame-rate limit values used by the limiter.
constexpr std::array<FpsLock::Field, 4> kFields = {{
    {0x041F9D90U, 0x220U, false},
    {0x041F9A60U, 0x0B0U, true},
    // The limiter itself (writing only the fields above does not change the frame rate). Its class is
    // common (~350 objects), so it is matched through matches_signature().
    {0x0445B050U, 0x048U, true},
    {0x0445B050U, 0x04CU, true},
}};

// A field whose vtable matches more objects than this is only trusted through its signature.
constexpr std::size_t kMaxPlainMatches = 8;
// Never write into more objects than this for one field.
constexpr std::size_t kMaxFilteredMatches = 4;

bool is_menu_option(float value) {
  for (const float option : {30.0F, 60.0F, 90.0F, 120.0F, 180.0F, 240.0F}) {
    if (value == option) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Extra checks used only when a vtable matches many objects (see docs/FPS_LOCK.md).
bool FpsLock::matches_signature(std::uintptr_t object, const Field& field) const {
  if (field.vtable_rva == 0x0445B050U) {
    // Limiter: the cap twice (+0x48, +0x4C), then a pointer to exe+0x445B108 at +0x50.
    // Both floats are written together, so this still holds after the mod changed the cap.
    struct {
      float first;
      float second;
      std::uint64_t pointer;
    } data{};
    if (!read_memory(object + 0x48, &data, sizeof(data))) {
      return false;
    }
    return data.first == data.second && is_menu_option(data.first) && data.pointer == image_base_ + 0x0445B108U;
  }
  // +0xB0 holder keeps the frame time in ms right after the cap (60.0 then 16.67).
  float pair[2]{};
  if (!read_memory(object + field.offset, pair, sizeof(pair))) {
    return false;
  }
  return is_menu_option(pair[0]) && std::fabs(pair[0] * pair[1] - 1000.0F) < 2.0F;
}

// Finds the objects whose first qword is one of the known vtables.
void FpsLock::rescan() {
  std::array<std::vector<std::uintptr_t>, kFields.size()> matches;
  auto* buffer = static_cast<std::byte*>(VirtualAlloc(nullptr, kChunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (buffer == nullptr) {
    return;
  }
  std::uintptr_t address = 0x10000;
  MEMORY_BASIC_INFORMATION info{};
  while (!stop_.load() && VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info)) {
    const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto region_end = region + info.RegionSize;
    if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE) {
      for (std::uintptr_t chunk = region; chunk < region_end; chunk += kChunkBytes) {
        const auto size = static_cast<std::size_t>(std::min<std::uintptr_t>(kChunkBytes, region_end - chunk));
        if (!read_memory(chunk, buffer, size)) {
          continue;
        }
        for (std::size_t offset = 0; offset + 8 <= size; offset += 8) {
          std::uint64_t value = 0;
          std::memcpy(&value, buffer + offset, sizeof(value));
          if (value < image_base_ || value >= image_end_) {
            continue;
          }
          for (std::size_t i = 0; i < kFields.size(); ++i) {
            if (value == image_base_ + kFields[i].vtable_rva) {
              matches[i].push_back(chunk + offset);
            }
          }
        }
      }
    }
    if (region_end <= address) {
      break;
    }
    address = region_end;
  }
  VirtualFree(buffer, 0, MEM_RELEASE);

  std::lock_guard lock(mutex_);
  // Keep objects found earlier while they are alive: once the mod writes its cap, the signature
  // (cap x frame time = 1000) no longer holds, so a rescan would not find them again.
  std::vector<Target> next;
  for (const auto& target : targets_) {
    if (still_valid(target)) {
      next.push_back(target);
    }
  }
  std::string summary;
  for (std::size_t i = 0; i < kFields.size(); ++i) {
    std::vector<std::uintptr_t> accepted;
    std::size_t signature_matches = matches[i].size();
    if (matches[i].size() <= kMaxPlainMatches) {
      accepted = matches[i];
    } else {
      for (const auto object : matches[i]) {
        if (matches_signature(object, kFields[i])) {
          accepted.push_back(object);
        }
      }
      signature_matches = accepted.size();
      if (accepted.size() > kMaxFilteredMatches) {
        accepted.clear();  // Still ambiguous: leave this field alone rather than guess.
      }
    }
    for (const auto object : accepted) {
      const bool known = std::any_of(next.begin(), next.end(), [&](const Target& t) {
        return t.object == object && t.field.offset == kFields[i].offset && t.field.vtable_rva == kFields[i].vtable_rva;
      });
      if (!known) {
        next.push_back(Target{object, kFields[i]});
      }
    }
    char part[128]{};
    std::snprintf(part, sizeof(part), "%s+0x%x: %zu objects, %zu signature, %zu used", summary.empty() ? "" : "; ",
                  kFields[i].offset, matches[i].size(), signature_matches, accepted.size());
    summary += part;
  }
  targets_ = std::move(next);
  objects_.store(static_cast<std::int32_t>(targets_.size()), std::memory_order_relaxed);
  if (log_ && summary != last_summary_) {
    log_("FpsLock scan: " + summary + "; tracking " + std::to_string(targets_.size()) + "; previous scan took " +
         std::to_string(static_cast<int>(last_scan_ms_)) + " ms");
    last_summary_ = summary;
  }
}

void FpsLock::loop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  auto next_scan = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::int32_t last_logged_target = -1;
  std::int32_t previous_target = target_.load();
  std::uint64_t writes = 0;

  while (!stop_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_scan) {
      const auto scan_started = std::chrono::steady_clock::now();
      rescan();
      last_scan_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_started).count();
      const auto count = objects_.load();
      // Objects appear while the game boots; look often until they exist, then every 10 s.
      next_scan = now + std::chrono::seconds(count >= 4 ? 10 : 3);
    }

    const auto target = target_.load(std::memory_order_relaxed);
    const auto writes_before = writes;
    const bool enforce = target != 0;
    const std::int32_t desired_value = target < 0 ? 0 : target;  // The game stores "unlimited" as 0.
    {
      std::lock_guard lock(mutex_);
      bool any_invalid = false;
      std::int32_t observed = -1;
      for (const auto& entry : targets_) {
        if (!still_valid(entry)) {
          any_invalid = true;
          continue;
        }
        const auto field_address = entry.object + entry.field.offset;
        if (entry.field.is_float) {
          float value = 0.0F;
          if (!read_memory(field_address, &value, sizeof(value)) || !std::isfinite(value) || value < 0.0F || value > 1000.0F) {
            continue;
          }
          if (observed < 0) {
            observed = static_cast<std::int32_t>(std::lround(value));
          }
          if (enforce && value != static_cast<float>(desired_value)) {
            const auto desired = static_cast<float>(desired_value);
            writes += write_memory(field_address, &desired, sizeof(desired)) ? 1 : 0;
          }
        } else {
          std::int32_t value = 0;
          if (!read_memory(field_address, &value, sizeof(value)) || value < 0 || value > 1000) {
            continue;
          }
          observed = value;  // The menu value is the most meaningful one to show.
          if (enforce && value != desired_value) {
            writes += write_memory(field_address, &desired_value, sizeof(desired_value)) ? 1 : 0;
          }
        }
      }
      game_value_.store(observed, std::memory_order_relaxed);
      // The game overwrote a field we keep (it resets the cap after the Rockstar sign-in and may
      // create a new limiter object at that point): look for new objects soon.
      const bool game_reset = writes != writes_before && target == previous_target;
      if (any_invalid || game_reset) {
        next_scan = std::min(next_scan, now + std::chrono::seconds(1));
      }
      previous_target = target;
    }
    if (target != last_logged_target && log_) {
      log_("FpsLock: target " +
           (target > 0 ? std::to_string(target) : std::string(target < 0 ? "unlimited" : "off (game decides)")) +
           ", writes so far " + std::to_string(writes));
      last_logged_target = target;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (log_) {
    log_("FpsLock: stopped after " + std::to_string(writes) + " writes");
  }
}

}  // namespace sade
