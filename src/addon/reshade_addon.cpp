// ReShade add-on front-end for the raw mouse fix.
//
// ReShade unloads and reloads add-ons whenever it recreates its device, so this module stays thin:
// it loads the core (GTASADE.PCFix.asi) once, never frees it, and only draws settings.
// The core installs its hooks into SanAndreas.exe and keeps running independently of this module.

#include "api/sade_api.h"

#include <Windows.h>
#include <shellapi.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "GTASADE PC Fix";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Raw Input mouse camera for GTA San Andreas - The Definitive Edition. Sensitivity, on/off, mouse info and "
    "developer traces.";

namespace {

HMODULE g_addon_module = nullptr;
std::mutex g_core_mutex;
std::atomic<bool> g_core_attempted{false};
sade::api::GetStatusFn g_get_status = nullptr;
sade::api::SetEnabledFn g_set_enabled = nullptr;
sade::api::SetGainFn g_set_gain = nullptr;
sade::api::SaveSettingsFn g_save_settings = nullptr;
sade::api::SetDeveloperModeFn g_set_developer_mode = nullptr;
sade::api::DevFrameFn g_dev_frame = nullptr;
sade::api::SetMaxFpsFn g_set_max_fps = nullptr;
sade::api::ScanStartFn g_scan_start = nullptr;
sade::api::ScanStatusFn g_scan_status = nullptr;
sade::api::ScanWriteFn g_scan_write = nullptr;
sade::api::ScanDumpFn g_scan_dump = nullptr;
int g_scan_value = 60;
int g_scan_write_value = 120;
std::string g_scan_message;
std::string g_core_error;

// UI state kept across frames.
bool g_link_axes = true;
double g_save_message_until = 0.0;
bool g_save_ok = false;
bool g_dev_directory_loaded = false;
char g_dev_directory[260] = {};

const ImVec4 kGood(0.45F, 0.85F, 0.45F, 1.0F);
const ImVec4 kWarn(0.95F, 0.65F, 0.30F, 1.0F);
const ImVec4 kBad(0.95F, 0.40F, 0.40F, 1.0F);

std::filesystem::path addon_directory() {
  wchar_t path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(g_addon_module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(path).parent_path();
}

std::wstring widen(const char* text) {
  const int chars = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  std::wstring out(static_cast<std::size_t>(chars > 0 ? chars : 1), L'\0');
  if (chars > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), chars);
  }
  out.resize(wcslen(out.c_str()));
  return out;
}

HMODULE find_or_load_core() {
  if (HMODULE loaded = GetModuleHandleW(sade::api::kCoreModuleName)) {
    return loaded;  // Already loaded, e.g. by an ASI loader.
  }
  const auto dir = addon_directory();
  for (const auto& candidate : {dir / sade::api::kCoreModuleName, dir / L"scripts" / sade::api::kCoreModuleName}) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      // Never freed on purpose: the core owns hooks that must outlive add-on reloads.
      if (HMODULE loaded = LoadLibraryW(candidate.c_str())) {
        return loaded;
      }
    }
  }
  return nullptr;
}

// Called outside of DllMain (device init, present or overlay draw), so LoadLibrary does not run
// under the loader lock.
void ensure_core() {
  std::lock_guard lock(g_core_mutex);
  if (g_core_attempted.load()) {
    return;
  }
  g_core_attempted.store(true);
  HMODULE core = find_or_load_core();
  if (core == nullptr) {
    g_core_error = "GTASADE.PCFix.asi was not found next to this add-on or in scripts\\.";
    return;
  }
  g_get_status = reinterpret_cast<sade::api::GetStatusFn>(GetProcAddress(core, "SadeGetStatus"));
  g_set_enabled = reinterpret_cast<sade::api::SetEnabledFn>(GetProcAddress(core, "SadeSetEnabled"));
  g_set_gain = reinterpret_cast<sade::api::SetGainFn>(GetProcAddress(core, "SadeSetGain"));
  g_save_settings = reinterpret_cast<sade::api::SaveSettingsFn>(GetProcAddress(core, "SadeSaveSettings"));
  g_set_developer_mode = reinterpret_cast<sade::api::SetDeveloperModeFn>(GetProcAddress(core, "SadeSetDeveloperMode"));
  g_dev_frame = reinterpret_cast<sade::api::DevFrameFn>(GetProcAddress(core, "SadeDevFrame"));
  g_set_max_fps = reinterpret_cast<sade::api::SetMaxFpsFn>(GetProcAddress(core, "SadeSetMaxFps"));
  // Optional developer tools; the panel hides them if missing.
  g_scan_start = reinterpret_cast<sade::api::ScanStartFn>(GetProcAddress(core, "SadeScanStart"));
  g_scan_status = reinterpret_cast<sade::api::ScanStatusFn>(GetProcAddress(core, "SadeScanStatus"));
  g_scan_write = reinterpret_cast<sade::api::ScanWriteFn>(GetProcAddress(core, "SadeScanWrite"));
  g_scan_dump = reinterpret_cast<sade::api::ScanDumpFn>(GetProcAddress(core, "SadeScanDump"));
  if (g_get_status == nullptr || g_set_enabled == nullptr || g_set_gain == nullptr || g_save_settings == nullptr ||
      g_set_developer_mode == nullptr || g_dev_frame == nullptr || g_set_max_fps == nullptr) {
    g_core_error = "The loaded GTASADE.PCFix.asi does not match this add-on version.";
    g_get_status = nullptr;
    g_dev_frame = nullptr;
  }
}

void on_init_device(reshade::api::device*) {
  ensure_core();
}

void on_present(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*,
                const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  if (!g_core_attempted.load(std::memory_order_relaxed)) {
    ensure_core();  // The add-on may have been reloaded after the device already existed.
  }
  if (g_dev_frame != nullptr) {
    g_dev_frame();  // Frame counter for developer traces; cheap when developer mode is off.
  }
}

// Two-column key/value rows; the table sizes the label column, so text never overlaps.
bool begin_info_table(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit, ImVec2(0.0F, 0.0F), 0.0F)) {
    return false;
  }
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 0.0F, 0);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.0F, 0);
  return true;
}

void info_row(const char* label, const ImVec4* color, const char* format, ...) {
  ImGui::TableNextRow(0, 0.0F);
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  char value[512]{};
  va_list args;
  va_start(args, format);
  std::vsnprintf(value, sizeof(value), format, args);
  va_end(args);
  if (color != nullptr) {
    ImGui::TextColored(*color, "%s", value);
  } else {
    ImGui::TextUnformatted(value);
  }
}

// ---- On-screen debug overlay -------------------------------------------------------------------
// Drawn every frame via the reshade_overlay event, also while the ReShade menu is closed. It only
// accepts mouse input (to be dragged) while the menu is open, so it never steals game input.

constexpr const char* kConfigSection = "GTASADE.PCFix";
constexpr int kHistory = 180;

bool g_osd_enabled = false;
bool g_osd_config_loaded = false;
bool g_menu_open = false;
int g_osd_x = 40;
int g_osd_y = 40;
bool g_osd_reset_position = false;

struct FrameSample {
  float frame_ms = 0.0F;
  float packets = 0.0F;  // Mouse reports that arrived during the frame.
};
FrameSample g_history[kHistory] = {};
int g_history_next = 0;
LARGE_INTEGER g_last_frame_qpc{};
std::int64_t g_last_packets = -1;
std::uint64_t g_last_applied = 0;
std::uint64_t g_last_stale = 0;
double g_window_start = 0.0;
std::uint64_t g_window_applied = 0;
std::uint64_t g_window_stale = 0;
float g_stale_percent = 0.0F;

void load_osd_config(reshade::api::effect_runtime* runtime) {
  if (g_osd_config_loaded) {
    return;
  }
  g_osd_config_loaded = true;
  reshade::get_config_value(runtime, kConfigSection, "OverlayEnabled", g_osd_enabled);
  reshade::get_config_value(runtime, kConfigSection, "OverlayX", g_osd_x);
  reshade::get_config_value(runtime, kConfigSection, "OverlayY", g_osd_y);
}

void save_osd_config(reshade::api::effect_runtime* runtime) {
  reshade::set_config_value(runtime, kConfigSection, "OverlayEnabled", g_osd_enabled);
  reshade::set_config_value(runtime, kConfigSection, "OverlayX", g_osd_x);
  reshade::set_config_value(runtime, kConfigSection, "OverlayY", g_osd_y);
}

void sample_frame(const sade::api::StatusV1& status) {
  LARGE_INTEGER now{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  const std::int64_t packets = status.input_source == 1 ? status.ingame_packets : status.companion_packets;
  if (g_last_frame_qpc.QuadPart != 0 && g_last_packets >= 0) {
    FrameSample& sample = g_history[g_history_next];
    sample.frame_ms = static_cast<float>(static_cast<double>(now.QuadPart - g_last_frame_qpc.QuadPart) * 1000.0 /
                                         static_cast<double>(frequency.QuadPart));
    sample.packets = static_cast<float>(packets >= g_last_packets ? packets - g_last_packets : 0);
    g_history_next = (g_history_next + 1) % kHistory;
  }
  g_last_frame_qpc = now;
  g_last_packets = packets;

  // Share of camera reads that found no new motion, over the last second of gameplay.
  if (status.applied >= g_last_applied && status.stale >= g_last_stale) {
    g_window_applied += status.applied - g_last_applied;
    g_window_stale += status.stale - g_last_stale;
  }
  g_last_applied = status.applied;
  g_last_stale = status.stale;
  if (ImGui::GetTime() - g_window_start >= 1.0) {
    const auto total = g_window_applied + g_window_stale;
    g_stale_percent = total > 0 ? static_cast<float>(100.0 * static_cast<double>(g_window_stale) / static_cast<double>(total)) : 0.0F;
    g_window_start = ImGui::GetTime();
    g_window_applied = 0;
    g_window_stale = 0;
  }
}

float history_value(void* data, int index) {
  const auto* member = static_cast<const int*>(data);
  const FrameSample& sample = g_history[(g_history_next + index) % kHistory];
  return *member == 0 ? sample.frame_ms : sample.packets;
}

// ---- Hotkeys ----------------------------------------------------------------------------------
// Quick settings without opening the ReShade menu. Polled once per frame, only while the game
// window is in front, acting on key presses (not while held).

constexpr int kFpsOptions[] = {0, 30, 60, 90, 120, 180, 240, -1};
std::string g_toast;
double g_toast_until = 0.0;
bool g_hotkey_down[4] = {};

bool game_in_foreground() {
  DWORD process_id = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process_id);
  return process_id == GetCurrentProcessId();
}

bool key_pressed(int slot, int vkey) {
  const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(vkey) & 0x8000) != 0;
  const bool pressed = down && !g_hotkey_down[slot];
  g_hotkey_down[slot] = down;
  return pressed;
}

std::string fps_label(int value) {
  return value == 0 ? "off (game decides)" : value < 0 ? "unlimited" : std::to_string(value);
}

void toast(const std::string& text) {
  g_toast = text;
  g_toast_until = ImGui::GetTime() + 2.0;
}

void handle_hotkeys(const sade::api::StatusV1& status, reshade::api::effect_runtime* runtime) {
  if (!game_in_foreground()) {
    return;
  }
  const bool up = key_pressed(0, VK_PRIOR);
  const bool down = key_pressed(1, VK_NEXT);
  if (up || down) {
    int index = 0;
    for (int i = 0; i < static_cast<int>(std::size(kFpsOptions)); ++i) {
      if (kFpsOptions[i] == status.max_fps) {
        index = i;
      }
    }
    index = std::clamp(index + (up ? 1 : -1), 0, static_cast<int>(std::size(kFpsOptions)) - 1);
    g_set_max_fps(kFpsOptions[index]);
    toast("FPS cap: " + fps_label(kFpsOptions[index]));
  }
  if (key_pressed(2, VK_END)) {
    g_set_enabled(status.enabled == 0);
    toast(status.enabled == 0 ? "Raw mouse camera: ON" : "Raw mouse camera: OFF");
  }
  if (key_pressed(3, VK_INSERT)) {
    g_osd_enabled = !g_osd_enabled;
    save_osd_config(runtime);
  }
}

void draw_osd(reshade::api::effect_runtime* runtime) {
  if (g_get_status == nullptr) {
    return;
  }
  sade::api::StatusV1 status{};
  if (!g_get_status(&status)) {
    return;
  }
  sample_frame(status);
  handle_hotkeys(status, runtime);
  if (!g_osd_enabled) {
    return;
  }

  if (g_osd_reset_position) {
    g_osd_x = 40;
    g_osd_y = 40;
  }
  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(g_osd_x), static_cast<float>(g_osd_y)),
                          g_osd_reset_position ? ImGuiCond_Always : ImGuiCond_FirstUseEver, ImVec2(0.0F, 0.0F));
  g_osd_reset_position = false;
  ImGui::SetNextWindowBgAlpha(0.55F);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse;
  if (!g_menu_open) {
    flags |= ImGuiWindowFlags_NoInputs;
  }
  if (ImGui::Begin("SADE Raw Mouse##osd", nullptr, flags)) {
    if (status.input_source == 1) {
      ImGui::TextColored(kGood, "Mouse: game process  %d Hz", status.polling_hz);
    } else {
      ImGui::TextColored(kWarn, "Mouse: helper (throttled)  %d Hz", status.polling_hz);
    }
    if (status.enabled == 0) {
      ImGui::TextColored(kBad, "Raw camera OFF");
    }
    ImGui::Text("Sens %.2f x %.2f   %s", status.gain_x, status.gain_y,
                status.gameplay_active != 0 ? "camera: gameplay" : "camera: menu");
    if (status.game_fps_setting >= 0) {
      ImGui::Text("FPS cap %s (game: %s)", fps_label(status.max_fps).c_str(),
                  status.game_fps_setting == 0 ? "unlimited" : std::to_string(status.game_fps_setting).c_str());
    } else {
      ImGui::Text("FPS cap %s (game setting not found yet)", fps_label(status.max_fps).c_str());
    }
    if (ImGui::GetTime() < g_toast_until) {
      ImGui::TextColored(kGood, "%s", g_toast.c_str());
    }

    // Recent frames: average and worst.
    float frame_sum = 0.0F;
    float frame_worst = 0.0F;
    float packets_sum = 0.0F;
    int empty_frames = 0;
    int moving_frames = 0;
    for (const FrameSample& sample : g_history) {
      frame_sum += sample.frame_ms;
      frame_worst = std::max(frame_worst, sample.frame_ms);
      packets_sum += sample.packets;
    }
    // A frame with no report while the frames around it had reports means the mouse data arrived unevenly.
    for (int i = 1; i + 1 < kHistory; ++i) {
      const FrameSample& before = g_history[(g_history_next + i - 1) % kHistory];
      const FrameSample& current = g_history[(g_history_next + i) % kHistory];
      const FrameSample& after = g_history[(g_history_next + i + 1) % kHistory];
      if (before.packets > 0.0F && after.packets > 0.0F) {
        ++moving_frames;
        if (current.packets == 0.0F) {
          ++empty_frames;
        }
      }
    }
    const float frame_avg = frame_sum / kHistory;
    ImGui::Text("FPS %.0f  frame %.1f ms  worst %.1f ms", frame_avg > 0.0F ? 1000.0F / frame_avg : 0.0F, frame_avg,
                frame_worst);
    const bool gap_bad = status.polling_hz > 0 && status.max_gap_us_last_second > 4 * 1000000 / status.polling_hz &&
                         status.max_gap_us_last_second > 4000;
    ImGui::Text("Reports/frame %.1f", packets_sum / kHistory);
    ImGui::SameLine(0.0F, -1.0F);
    ImGui::TextColored(gap_bad ? kWarn : ImVec4(1, 1, 1, 1), " max gap %.1f ms", status.max_gap_us_last_second / 1000.0);
    const float empty_percent = moving_frames > 0 ? 100.0F * empty_frames / moving_frames : 0.0F;
    ImGui::TextColored(empty_percent > 10.0F ? kWarn : ImVec4(1, 1, 1, 1), "Frames without input while moving %.0f%%",
                       empty_percent);
    if (status.gameplay_active != 0) {
      ImGui::Text("Camera reads with no new motion %.0f%%", g_stale_percent);
    }

    static int frame_member = 0;
    static int packets_member = 1;
    ImGui::PlotLines("##frame_ms", history_value, &frame_member, kHistory, 0, "frame ms", 0.0F, 50.0F, ImVec2(260.0F, 36.0F));
    ImGui::PlotHistogram("##packets", history_value, &packets_member, kHistory, 0, "reports/frame", 0.0F, 20.0F,
                         ImVec2(260.0F, 36.0F));

    ImGui::TextDisabled("Ctrl+PgUp/PgDn FPS  Ctrl+End mouse  Ctrl+Ins hide");
    if (g_menu_open) {
      ImGui::TextDisabled("Drag to move (ReShade menu is open)");
      const ImVec2 position = ImGui::GetWindowPos();
      const int x = static_cast<int>(position.x);
      const int y = static_cast<int>(position.y);
      if (x != g_osd_x || y != g_osd_y) {
        g_osd_x = x;
        g_osd_y = y;
        save_osd_config(runtime);
      }
    }
  }
  ImGui::End();
}

void on_reshade_overlay(reshade::api::effect_runtime* runtime) {
  if (!g_core_attempted.load(std::memory_order_relaxed)) {
    ensure_core();
  }
  load_osd_config(runtime);
  draw_osd(runtime);
}

bool on_open_overlay(reshade::api::effect_runtime*, bool open, reshade::api::input_source) {
  g_menu_open = open;
  return false;
}

void draw_sensitivity(const sade::api::StatusV1& status) {
  float gain_x = status.gain_x;
  float gain_y = status.gain_y;
  bool changed = false;
  const float input_width = ImGui::CalcTextSize("00.000", nullptr, false, -1.0F).x + ImGui::GetFrameHeight() * 2.5F;

  auto axis = [&](const char* slider_id, const char* input_id, float& value) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - input_width - ImGui::GetStyle().ItemSpacing.x);
    changed |= ImGui::SliderFloat(slider_id, &value, 0.1F, 5.0F, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(0.0F, -1.0F);
    ImGui::SetNextItemWidth(input_width);
    // Exact value: type a number, or use +/- (Ctrl for bigger steps).
    changed |= ImGui::InputFloat(input_id, &value, 0.01F, 0.1F, "%.2f", 0);
  };

  ImGui::Checkbox("Same sensitivity for X and Y", &g_link_axes);
  if (g_link_axes) {
    ImGui::TextUnformatted("Sensitivity");
    axis("##sens", "##sens_exact", gain_x);
    if (changed) {
      gain_y = gain_x;
    }
  } else {
    ImGui::TextUnformatted("Sensitivity X (horizontal)");
    axis("##sens_x", "##sens_x_exact", gain_x);
    ImGui::TextUnformatted("Sensitivity Y (vertical)");
    axis("##sens_y", "##sens_y_exact", gain_y);
  }
  if (changed) {
    g_set_gain(gain_x, gain_y);
  }

  if (ImGui::Button("Reset to 1.00", ImVec2(0.0F, 0.0F))) {
    g_set_gain(1.0F, 1.0F);
  }
  ImGui::SameLine(0.0F, -1.0F);
  if (ImGui::Button("Save to ini", ImVec2(0.0F, 0.0F))) {
    g_save_ok = g_save_settings();
    g_save_message_until = ImGui::GetTime() + 3.0;
  }
  if (ImGui::GetTime() < g_save_message_until) {
    ImGui::SameLine(0.0F, -1.0F);
    ImGui::TextColored(g_save_ok ? kGood : kBad, "%s", g_save_ok ? "Saved." : "Could not write the ini file.");
  }
}

void draw_status(const sade::api::StatusV1& status, reshade::api::effect_runtime* runtime) {
  if (!begin_info_table("##status")) {
    return;
  }
  if (status.supported_build != 0) {
    info_row("Game build", &kGood, "%s", status.game_version);
  } else {
    info_row("Game build", &kBad, "%s - unsupported, the fix is inactive", status.game_version);
  }
  info_row("Core", status.running != 0 ? &kGood : &kBad, "%s", status.running != 0 ? status.core_version : "not running");
  info_row("Raw Input helper", status.companion_running != 0 ? &kGood : &kBad, "%s",
           status.companion_running != 0 ? "running" : "not running");
  info_row("Camera hooks", status.hooks_active != 0 ? &kGood : &kWarn, "%s",
           status.hooks_active != 0 ? "active" : "waiting (about 15 s after launch)");
  info_row("Gameplay camera", status.gameplay_active != 0 ? &kGood : nullptr, "%s",
           status.gameplay_active != 0 ? "captured" : "idle (menu, map or cutscene)");
  if (runtime != nullptr) {
    uint32_t width = 0;
    uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    info_row("Render size", nullptr, "%u x %u", width, height);
  }
  info_row("Camera updates", nullptr, "applied %llu, no new motion %llu, passed to game %llu",
           static_cast<unsigned long long>(status.applied), static_cast<unsigned long long>(status.stale),
           static_cast<unsigned long long>(status.passthrough));
  ImGui::EndTable();
}

void draw_mouse(const sade::api::StatusV1& status) {
  if (!begin_info_table("##mouse")) {
    return;
  }
  info_row("Device", nullptr, "%s", status.device_name[0] != '\0' ? status.device_name : "move the mouse to detect");
  if (status.input_source == 1) {
    info_row("Input source", &kGood, "%s", status.sink_registered != 0 ? "game process, mod window (full rate)"
                                                                      : "game process, game window (full rate)");
  } else if (status.ingame_hook_installed != 0) {
    info_row("Input source", &kWarn, "helper process (game process silent, fallback)");
  } else {
    info_row("Input source", &kWarn, "helper process (game hook not ready yet)");
  }
  if (status.polling_hz > 0) {
    info_row("Polling rate", nullptr, "~%d Hz (measured while moving)", status.polling_hz);
  } else {
    info_row("Polling rate", nullptr, "move the mouse to measure");
  }
  info_row("Reports last second", nullptr, "%d", status.packets_last_second);
  const double max_gap_ms = status.max_gap_us_last_second / 1000.0;
  const double expected_ms = status.polling_hz > 0 ? 1000.0 / status.polling_hz : 0.0;
  const bool gap_suspicious = expected_ms > 0.0 && max_gap_ms > expected_ms * 4.0 && max_gap_ms > 4.0;
  info_row("Longest gap last second", gap_suspicious ? &kWarn : nullptr, "%.2f ms", max_gap_ms);
  info_row("Total reports", nullptr, "game %lld, helper %lld", static_cast<long long>(status.ingame_packets),
           static_cast<long long>(status.companion_packets));
  if (status.companion_polling_hz > 0) {
    // Windows coalesces raw input for background processes, so the helper usually sees ~125 Hz.
    info_row("Helper process rate", nullptr, "~%d Hz (Windows limits background apps)", status.companion_polling_hz);
  }

  // Windows pointer settings only affect the menu cursor, not the raw camera input.
  int pointer_speed = 0;
  int mouse_params[3]{};
  SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &pointer_speed, 0);
  SystemParametersInfoW(SPI_GETMOUSE, 0, mouse_params, 0);
  info_row("Windows pointer speed", pointer_speed == 10 ? nullptr : &kWarn, "%d / 20%s", pointer_speed,
           pointer_speed == 10 ? " (default)" : " (menu cursor only)");
  info_row("Enhance pointer precision", mouse_params[2] != 0 ? &kWarn : nullptr, "%s",
           mouse_params[2] != 0 ? "on (menu cursor only)" : "off");
  ImGui::EndTable();
}

// Finds where the game keeps a setting (the frame-rate cap): scan for the value shown in the game
// menu, change it in the menu, scan again, until a few addresses are left.
void draw_value_finder() {
  if (g_scan_start == nullptr || g_scan_status == nullptr || g_scan_write == nullptr || g_scan_dump == nullptr) {
    return;
  }
  ImGui::SeparatorText("Setting finder (FPS cap)");
  sade::api::ScanStatusV1 scan{};
  g_scan_status(&scan);
  const auto total = scan.int_candidates + scan.float_candidates + scan.double_candidates;

  ImGui::TextWrapped("1. Pick an FPS cap in the game menu.  2. Type the same number here and press First scan.  "
                     "3. Pick another cap in the game, type it, press Next scan.  Repeat until few candidates are left.");
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000", nullptr, false, -1.0F).x + ImGui::GetFrameHeight() * 2.5F);
  ImGui::InputInt("Value in game menu now", &g_scan_value, 1, 10, 0);
  ImGui::BeginDisabled(scan.busy != 0);
  if (ImGui::Button("First scan", ImVec2(0.0F, 0.0F))) {
    g_scan_start(static_cast<double>(g_scan_value), true);
  }
  ImGui::SameLine(0.0F, -1.0F);
  ImGui::BeginDisabled(scan.scans_done == 0);
  if (ImGui::Button("Next scan", ImVec2(0.0F, 0.0F))) {
    g_scan_start(static_cast<double>(g_scan_value), false);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  if (scan.busy != 0) {
    ImGui::TextColored(kWarn, "Scanning... the game may stutter for a few seconds.");
  } else if (scan.scans_done > 0) {
    ImGui::Text("Scans %d, candidates: int %llu, float %llu, double %llu  (%.1f s, %.0f MB)", scan.scans_done,
                static_cast<unsigned long long>(scan.int_candidates), static_cast<unsigned long long>(scan.float_candidates),
                static_cast<unsigned long long>(scan.double_candidates), scan.last_scan_seconds,
                static_cast<double>(scan.bytes_scanned) / (1024.0 * 1024.0));
    if (total == 0) {
      ImGui::TextColored(kWarn, "Nothing left. The game may store the menu position instead of the number:");
      ImGui::TextColored(kWarn, "try again entering the option's position (0 = first option, 1 = second, ...).");
    }
  }

  ImGui::BeginDisabled(scan.busy != 0 || total == 0 || total > 64);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000", nullptr, false, -1.0F).x + ImGui::GetFrameHeight() * 2.5F);
  ImGui::InputInt("Test value", &g_scan_write_value, 1, 10, 0);
  ImGui::SameLine(0.0F, -1.0F);
  if (ImGui::Button("Write to candidates", ImVec2(0.0F, 0.0F))) {
    const int written = g_scan_write(static_cast<double>(g_scan_write_value));
    char text[128]{};
    std::snprintf(text, sizeof(text), "Wrote to %d addresses - watch the FPS in the info box.", written);
    g_scan_message = text;
  }
  ImGui::SameLine(0.0F, -1.0F);
  if (ImGui::Button("Save to log", ImVec2(0.0F, 0.0F))) {
    char text[128]{};
    std::snprintf(text, sizeof(text), "Saved %d candidates to the log.", g_scan_dump());
    g_scan_message = text;
  }
  ImGui::EndDisabled();
  if (total > 64 && scan.busy == 0) {
    ImGui::TextDisabled("Write and Save unlock at 64 candidates or fewer.");
  }
  if (!g_scan_message.empty()) {
    ImGui::TextUnformatted(g_scan_message.c_str());
  }
}

void draw_developer(const sade::api::StatusV1& status) {
  if (!g_dev_directory_loaded) {
    std::snprintf(g_dev_directory, sizeof(g_dev_directory), "%s", status.developer_log_directory);
    g_dev_directory_loaded = true;
  }
  bool configured = status.developer_mode_configured != 0;
  if (ImGui::Checkbox("Developer mode (applies on next launch)", &configured)) {
    g_set_developer_mode(configured, widen(g_dev_directory).c_str());
  }
  ImGui::TextDisabled("Writes full mouse/camera traces for every run. Leave off for normal play.");
  ImGui::TextUnformatted("Log folder (empty = scripts\\GTASADE.PCFix_logs)");
  ImGui::SetNextItemWidth(-1.0F);
  if (ImGui::InputText("##devdir", g_dev_directory, sizeof(g_dev_directory), ImGuiInputTextFlags_EnterReturnsTrue, nullptr,
                       nullptr) ||
      ImGui::IsItemDeactivatedAfterEdit()) {
    g_set_developer_mode(configured, widen(g_dev_directory).c_str());
  }
  if (status.developer_mode_active != 0) {
    ImGui::TextColored(kGood, "Recording this run to:");
    ImGui::TextWrapped("%s", status.developer_run_directory);
    if (ImGui::Button("Open run folder", ImVec2(0.0F, 0.0F))) {
      ShellExecuteW(nullptr, L"open", widen(status.developer_run_directory).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    draw_value_finder();
  } else if (configured) {
    ImGui::TextColored(kWarn, "Enabled - restart the game to start recording.");
  }
}

// FPS cap kept by the mod. The game resets its own cap to 60 on every launch; the mod rewrites it.
void draw_fps_cap(const sade::api::StatusV1& status) {
  ImGui::TextUnformatted("FPS cap (kept by the mod, applies immediately)");
  static constexpr int kOptions[] = {0, 30, 60, 90, 120, 180, 240, -1};
  for (int option : kOptions) {
    char label[32]{};
    if (option == 0) {
      std::snprintf(label, sizeof(label), "Off##fps");
    } else if (option < 0) {
      std::snprintf(label, sizeof(label), "Unlimited##fps");
    } else {
      std::snprintf(label, sizeof(label), "%d##fps", option);
    }
    const bool selected = status.max_fps == option;
    if (selected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25F, 0.55F, 0.25F, 1.0F));
    }
    if (ImGui::Button(label, ImVec2(0.0F, 0.0F))) {
      g_set_max_fps(option);
    }
    if (selected) {
      ImGui::PopStyleColor(1);
    }
    ImGui::SameLine(0.0F, -1.0F);
  }
  ImGui::NewLine();
  if (status.max_fps == 0) {
    ImGui::TextDisabled("Off: the game's own setting is used (the game resets it to 60 on launch).");
  }
  if (status.game_fps_setting >= 0) {
    ImGui::TextDisabled("Game setting now: %s, fields found: %d",
                        status.game_fps_setting == 0 ? "unlimited" : std::to_string(status.game_fps_setting).c_str(),
                        status.fps_lock_objects);
  } else {
    ImGui::TextDisabled("Game setting not found yet (it appears a few seconds after the game starts).");
  }
}

void draw_settings(reshade::api::effect_runtime* runtime) {
  ensure_core();
  if (g_get_status == nullptr) {
    ImGui::TextColored(kBad, "%s", g_core_error.c_str());
    return;
  }

  sade::api::StatusV1 status{};
  if (!g_get_status(&status)) {
    ImGui::TextUnformatted("Core status unavailable.");
    return;
  }

  bool enabled = status.enabled != 0;
  if (ImGui::Checkbox("Raw mouse camera", &enabled)) {
    g_set_enabled(enabled);
  }
  ImGui::Spacing();
  draw_sensitivity(status);

  ImGui::Spacing();
  ImGui::Spacing();
  load_osd_config(runtime);
  if (ImGui::Checkbox("Show mouse info over the game", &g_osd_enabled)) {
    save_osd_config(runtime);
  }
  ImGui::SameLine(0.0F, -1.0F);
  if (ImGui::Button("Reset position", ImVec2(0.0F, 0.0F))) {
    g_osd_reset_position = true;
    save_osd_config(runtime);
  }
  if (g_osd_enabled) {
    ImGui::TextDisabled("Drag the info box while this menu is open.");
  }
  ImGui::TextDisabled("Hotkeys: Ctrl+PgUp/PgDn FPS cap, Ctrl+End raw mouse on/off, Ctrl+Insert info box.");


  ImGui::Spacing();
  draw_fps_cap(status);

  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
    draw_status(status, runtime);
  }
  if (ImGui::CollapsingHeader("Mouse", ImGuiTreeNodeFlags_DefaultOpen)) {
    draw_mouse(status);
  }
  if (ImGui::CollapsingHeader("Developer", status.developer_mode_configured != 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
    draw_developer(status);
  }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      g_addon_module = module;
      if (!reshade::register_addon(module)) {
        return FALSE;
      }
      reshade::register_event<reshade::addon_event::init_device>(on_init_device);
      reshade::register_event<reshade::addon_event::present>(on_present);
      reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
      reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
      reshade::register_overlay(nullptr, draw_settings);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, draw_settings);
      reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
      reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
      reshade::unregister_event<reshade::addon_event::present>(on_present);
      reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
      reshade::unregister_addon(module);
      break;
    default:
      break;
  }
  return TRUE;
}
