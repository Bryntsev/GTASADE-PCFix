#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace sade {

constexpr std::uint32_t kExternalRawInputMagic = 0x53414445U;
constexpr std::uint32_t kExternalRawInputVersion = 2U;
constexpr std::size_t kExternalRawInputDeviceNameChars = 128;

// Shared only between the ASI and its companion process. All mutable fields use
// Win32 interlocked operations, so neither side needs to block the input path.
struct ExternalRawInputShared {
  std::uint32_t magic = kExternalRawInputMagic;
  std::uint32_t version = kExternalRawInputVersion;
  volatile LONG64 pending_x = 0;
  volatile LONG64 pending_y = 0;
  volatile LONG64 last_qpc = 0;
  volatile LONG64 packet_count = 0;

  // Version 2: mouse statistics measured by the companion, for display only.
  volatile LONG polling_hz = 0;              // Estimated report rate while the mouse moves.
  volatile LONG max_gap_us_last_second = 0;  // Largest gap between reports while moving, last second.
  volatile LONG packets_last_second = 0;
  volatile LONG device_generation = 0;       // Odd while device_name is being rewritten.
  wchar_t device_name[kExternalRawInputDeviceNameChars] = {};
};

}  // namespace sade
