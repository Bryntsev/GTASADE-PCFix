#pragma once

#include <cstdint>

// C API exported by the core module (GTASADE.PCFix.asi) so that front-ends such as the
// ReShade add-on can show status and change settings without linking against the core.
// Structs are versioned by their leading size field; only append new fields.

namespace sade::api {

constexpr const wchar_t* kCoreModuleName = L"GTASADE.PCFix.asi";

struct StatusV1 {
  std::uint32_t size = sizeof(StatusV1);
  std::int32_t running = 0;            // Core runtime started in this process.
  std::int32_t enabled = 0;            // User toggle; when off the game sees its own cursor input.
  std::int32_t supported_build = 0;    // SanAndreas.exe matches the build the RVAs were taken from.
  std::int32_t mode = 0;               // ExperimentalMouseFixMode in effect.
  std::int32_t hooks_active = 0;       // Delayed cursor hooks are installed.
  std::int32_t companion_running = 0;  // Raw Input companion process is alive.
  std::int32_t gameplay_active = 0;    // Camera capture was seen recently.
  float gain_x = 1.0F;
  float gain_y = 1.0F;
  std::int64_t companion_packets = 0;
  std::uint64_t applied = 0;
  std::uint64_t passthrough = 0;
  std::uint64_t stale = 0;
  char core_version[32] = {};
  char game_version[32] = {};

  // Mouse statistics measured by the companion.
  std::int32_t polling_hz = 0;
  std::int32_t packets_last_second = 0;
  std::int32_t max_gap_us_last_second = 0;
  char device_name[128] = {};  // UTF-8.

  // Developer mode. "configured" is what the ini says (applies on next launch).
  std::int32_t developer_mode_active = 0;
  std::int32_t developer_mode_configured = 0;
  char developer_log_directory[260] = {};  // UTF-8, as configured (empty = default).
  char developer_run_directory[260] = {};  // UTF-8, folder of the current run when active.

  // Input source. polling_hz/packets/max_gap above describe the source in use; the companion's own
  // numbers are kept for comparison (Windows coalesces background raw input to ~125 Hz).
  std::int32_t input_source = 0;  // 1 = game window (full rate), 2 = companion process.
  std::int32_t ingame_hook_installed = 0;
  std::int32_t companion_polling_hz = 0;
  std::int64_t ingame_packets = 0;
  std::int32_t sink_registered = 0;         // Our in-process window holds the mouse registration.
  std::int32_t recenter_interval_us = 0;    // Smoothed time between gameplay recenters (~frame time).
  std::int32_t max_fps = 0;                 // FPS cap the mod keeps (0 = the game decides, -1 = unlimited).
  std::int32_t fps_lock_objects = 0;        // Game setting fields found in memory.
  std::int32_t game_fps_setting = -1;       // The game's current cap (-1 = not found yet, 0 = unlimited).
};

using GetStatusFn = bool (*)(StatusV1* status);
using SetEnabledFn = void (*)(bool enabled);
using SetGainFn = void (*)(float gain_x, float gain_y);
using SaveSettingsFn = bool (*)();
using SetDeveloperModeFn = bool (*)(bool enabled, const wchar_t* log_directory);  // Writes the ini.
using DevFrameFn = void (*)();  // Frame marker for developer traces (called on present).
using SetMaxFpsFn = bool (*)(std::int32_t max_fps);  // 0 = leave the game alone, -1 = unlimited. Applies immediately.

// Developer value scanner (used to locate the game's frame-rate setting in memory).
struct ScanStatusV1 {
  std::uint32_t size = sizeof(ScanStatusV1);
  std::int32_t busy = 0;
  std::int32_t scans_done = 0;
  std::uint64_t int_candidates = 0;
  std::uint64_t float_candidates = 0;
  std::uint64_t double_candidates = 0;
  std::uint64_t bytes_scanned = 0;
  double last_scan_seconds = 0.0;
};

using ScanStartFn = bool (*)(double value, bool first);
using ScanStatusFn = bool (*)(ScanStatusV1* status);
using ScanWriteFn = std::int32_t (*)(double value);  // -1 when there are too many candidates.
using ScanDumpFn = std::int32_t (*)();               // Writes candidate details to the log.

}  // namespace sade::api

// Exported names: SadeGetStatus, SadeSetEnabled, SadeSetGain, SadeSaveSettings,
// SadeSetDeveloperMode, SadeDevFrame, SadeSetMaxFps, SadeScanStart, SadeScanStatus, SadeScanWrite,
// SadeScanDump.
