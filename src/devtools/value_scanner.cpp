#include "devtools/value_scanner.h"

#include <Windows.h>

#include <chrono>
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

// Plain heap memory only: write-combined or uncached pages are GPU mappings and are very slow to read.
bool scannable(const MEMORY_BASIC_INFORMATION& info) {
  return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE;
}

bool matches(ValueScanner::Kind kind, const std::byte* data, double value) {
  switch (kind) {
    case ValueScanner::Int32: {
      std::int32_t v = 0;
      std::memcpy(&v, data, sizeof(v));
      return static_cast<double>(v) == value;
    }
    case ValueScanner::Float: {
      float v = 0.0F;
      std::memcpy(&v, data, sizeof(v));
      return v == static_cast<float>(value);
    }
    case ValueScanner::Double: {
      double v = 0.0;
      std::memcpy(&v, data, sizeof(v));
      return v == value;
    }
    default:
      return false;
  }
}

std::size_t kind_size(ValueScanner::Kind kind) {
  return kind == ValueScanner::Double ? 8 : 4;
}

const char* kind_name(ValueScanner::Kind kind) {
  switch (kind) {
    case ValueScanner::Int32:
      return "int32";
    case ValueScanner::Float:
      return "float";
    default:
      return "double";
  }
}

}  // namespace

ValueScanner::~ValueScanner() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool ValueScanner::start_scan(double value, bool first) {
  bool expected = false;
  if (!busy_.compare_exchange_strong(expected, true)) {
    return false;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  worker_ = std::thread([this, value, first] { run(value, first); });
  return true;
}

void ValueScanner::run(double value, bool first) {
  const auto started = std::chrono::steady_clock::now();
  std::array<std::vector<std::uintptr_t>, KindCount> next;
  std::uint64_t bytes = 0;

  if (first) {
    // Scan buffer allocated separately so its own region can be skipped (it holds copies of values).
    auto* buffer = static_cast<std::byte*>(VirtualAlloc(nullptr, kChunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer != nullptr) {
      const auto buffer_address = reinterpret_cast<std::uintptr_t>(buffer);
      std::uintptr_t address = 0x10000;
      MEMORY_BASIC_INFORMATION info{};
      while (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info)) {
        const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto region_end = region + info.RegionSize;
        if (scannable(info) && !(buffer_address >= region && buffer_address < region_end)) {
          for (std::uintptr_t chunk = region; chunk < region_end; chunk += kChunkBytes) {
            const std::size_t size = static_cast<std::size_t>(std::min<std::uintptr_t>(kChunkBytes, region_end - chunk));
            if (!read_memory(chunk, buffer, size)) {
              continue;
            }
            bytes += size;
            for (std::size_t offset = 0; offset + 4 <= size; offset += 4) {
              const std::byte* data = buffer + offset;
              if (matches(Int32, data, value)) {
                next[Int32].push_back(chunk + offset);
              }
              if (matches(Float, data, value)) {
                next[Float].push_back(chunk + offset);
              }
              if ((offset % 8) == 0 && offset + 8 <= size && matches(Double, data, value)) {
                next[Double].push_back(chunk + offset);
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
    }
  } else {
    std::array<std::vector<std::uintptr_t>, KindCount> previous;
    {
      std::lock_guard lock(mutex_);
      previous = lists_;
    }
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
      for (const auto address : previous[kind]) {
        std::byte data[8]{};
        if (read_memory(address, data, kind_size(static_cast<Kind>(kind))) && matches(static_cast<Kind>(kind), data, value)) {
          next[kind].push_back(address);
        }
      }
    }
  }

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  {
    std::lock_guard lock(mutex_);
    lists_ = std::move(next);
    scans_done_ = first ? 1 : scans_done_ + 1;
    if (first) {
      bytes_scanned_ = bytes;
    }
    last_scan_seconds_ = elapsed;
  }
  busy_.store(false);
}

ValueScanner::Status ValueScanner::status() const {
  Status out;
  out.busy = busy_.load();
  std::lock_guard lock(mutex_);
  out.scans_done = scans_done_;
  for (std::size_t kind = 0; kind < KindCount; ++kind) {
    out.counts[kind] = lists_[kind].size();
  }
  out.bytes_scanned = bytes_scanned_;
  out.last_scan_seconds = last_scan_seconds_;
  return out;
}

int ValueScanner::write_all(double value, std::size_t limit) {
  if (busy_.load()) {
    return -1;
  }
  std::lock_guard lock(mutex_);
  std::size_t total = 0;
  for (const auto& list : lists_) {
    total += list.size();
  }
  if (total == 0 || total > limit) {
    return -1;
  }
  int written = 0;
  for (const auto address : lists_[Int32]) {
    const auto v = static_cast<std::int32_t>(value);
    written += write_memory(address, &v, sizeof(v)) ? 1 : 0;
  }
  for (const auto address : lists_[Float]) {
    const auto v = static_cast<float>(value);
    written += write_memory(address, &v, sizeof(v)) ? 1 : 0;
  }
  for (const auto address : lists_[Double]) {
    written += write_memory(address, &value, sizeof(value)) ? 1 : 0;
  }
  return written;
}

std::vector<ValueScanner::Candidate> ValueScanner::candidates(std::size_t limit) const {
  std::vector<Candidate> out;
  std::lock_guard lock(mutex_);
  for (std::size_t kind = 0; kind < KindCount; ++kind) {
    for (const auto address : lists_[kind]) {
      if (out.size() >= limit) {
        return out;
      }
      out.push_back(Candidate{address, static_cast<Kind>(kind)});
    }
  }
  return out;
}

std::string describe_candidate(const ValueScanner::Candidate& candidate, std::uintptr_t image_base, std::uintptr_t image_end) {
  char line[256]{};
  MEMORY_BASIC_INFORMATION info{};
  VirtualQuery(reinterpret_cast<const void*>(candidate.address), &info, sizeof(info));
  std::snprintf(line, sizeof(line), "candidate %s @0x%llx region_base=0x%llx alloc_base=0x%llx size=0x%llx", kind_name(candidate.kind),
                static_cast<unsigned long long>(candidate.address),
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.BaseAddress)),
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.AllocationBase)),
                static_cast<unsigned long long>(info.RegionSize));
  std::string out = line;

  // Pointers into SanAndreas.exe within 0x1000 bytes before the value: the nearest one is most
  // likely the vtable of the object that owns the field.
  int found = 0;
  for (std::uintptr_t back = 0; back <= 0x1000 && found < 6; back += 8) {
    const std::uintptr_t slot = (candidate.address & ~static_cast<std::uintptr_t>(7)) - back;
    std::uint64_t value = 0;
    if (!read_memory(slot, &value, sizeof(value))) {
      break;
    }
    if (value >= image_base && value < image_end) {
      std::snprintf(line, sizeof(line), "\n    -0x%llx: exe+0x%llx", static_cast<unsigned long long>(candidate.address - slot),
                    static_cast<unsigned long long>(value - image_base));
      out += line;
      ++found;
    }
  }

  // Raw bytes around the value for manual inspection.
  std::byte bytes[64]{};
  if (read_memory(candidate.address - 32, bytes, sizeof(bytes))) {
    out += "\n    bytes[-32..+32]:";
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
      std::snprintf(line, sizeof(line), "%s%02x", (i % 8) == 0 ? " " : "", static_cast<unsigned>(bytes[i]));
      out += line;
    }
  }
  return out;
}

}  // namespace sade
