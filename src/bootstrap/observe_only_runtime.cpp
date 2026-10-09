#include "bootstrap/observe_only_runtime.h"

#include "api/sade_api.h"

#include "common/build_fingerprint.h"
#include "common/config.h"
#include "common/csv_trace.h"
#include "common/file_util.h"
#include "common/logger.h"
#include "devtools/value_scanner.h"
#include "game/fps_lock.h"
#include "hooks/iat_hook.h"
#include "input/raw_input_observer.h"
#include "input/raw_input_queue.h"
#include "input/external_raw_input_shared.h"
#include "input/mouse_rate_meter.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <intrin.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <mutex>
#include <thread>

namespace sade {
namespace {

Logger g_logger;
CsvTrace g_raw_csv;
CsvTrace g_timing_csv;
CsvTrace g_internal_a_csv;
CsvTrace g_marker_csv;
std::unique_ptr<RawInputQueue> g_queue;
std::unique_ptr<RawInputObserver> g_observer;
IatHookSet g_get_raw_input_data_hooks;
IatHookSet g_get_raw_input_buffer_hooks;
IatHookSet g_register_raw_input_devices_hooks;
IatHookSet g_get_proc_address_hooks;
IatHookSet g_get_cursor_pos_hooks;
IatHookSet g_set_cursor_pos_hooks;
IatHookSet g_get_async_key_state_hooks;
IatHookSet g_get_key_state_hooks;
IatHookSet g_clip_cursor_hooks;
IatHookSet g_get_clip_cursor_hooks;
std::thread g_timing_thread;
std::thread g_internal_trace_thread;
std::thread g_marker_thread;
std::thread g_raw_input_sink_thread;
std::atomic<bool> g_stop_timing{false};
std::atomic<bool> g_stop_internal_trace{false};
std::atomic<bool> g_stop_markers{false};
std::atomic<bool> g_stop_raw_input_sink{false};
std::atomic<bool> g_observe_internal_site12{false};
std::atomic<bool> g_observe_internal_site13{false};
std::atomic<std::uint32_t> g_current_segment_id{0};
std::atomic<bool> g_observe_get_proc_address{false};
std::atomic<bool> g_observe_cursor_apis{false};
std::atomic<bool> g_observe_capture_apis{false};
std::atomic<bool> g_observe_gameplay_state{false};
std::atomic<bool> g_experimental_mouse_fix{false};
std::atomic<std::uint32_t> g_experimental_mouse_fix_mode{2};
std::atomic<std::uint32_t> g_experimental_mouse_fix_max_age_ms{50};
std::atomic<std::int32_t> g_experimental_mouse_fix_center_x{1920};
std::atomic<std::int32_t> g_experimental_mouse_fix_center_y{1080};
std::atomic<std::uint32_t> g_experimental_mouse_fix_gain_x_bits{0};
std::atomic<std::uint32_t> g_experimental_mouse_fix_gain_y_bits{0};
std::atomic<LONGLONG> g_candidate_b_cursor_qpc{0};
std::atomic<std::int32_t> g_candidate_b_cursor_x{0};
std::atomic<std::int32_t> g_candidate_b_cursor_y{0};
std::atomic<std::uint64_t> g_experimental_patch_applied{0};
std::atomic<std::uint64_t> g_experimental_patch_passthrough{0};
std::atomic<std::uint64_t> g_experimental_patch_stale{0};
std::atomic<std::uint64_t> g_experimental_patch_invalid{0};
std::atomic<bool> g_delayed_cursor_filter_active{false};
std::atomic<std::int32_t> g_experimental_virtual_cursor_x{1920};
std::atomic<std::int32_t> g_experimental_virtual_cursor_y{1080};
std::atomic<std::uint32_t> g_experimental_virtual_cursor_idle_calls{0};
std::atomic<bool> g_experimental_rinput_protocol_active{false};
std::atomic<LONGLONG> g_experimental_last_center_warp_qpc{0};
std::atomic<std::uint32_t> g_experimental_center_warp_burst_count{0};
std::atomic<LONGLONG> g_gameplay_state_last_center_warp_qpc{0};
std::atomic<double> g_recenter_interval_ticks{0.0};
std::atomic<std::uint32_t> g_gameplay_state_center_warp_burst_count{0};
std::atomic<std::uint64_t> g_gameplay_state_set_total{0};
std::atomic<std::uint64_t> g_gameplay_state_center_like{0};
std::atomic<std::uint64_t> g_gameplay_state_gameplay_like{0};
std::atomic<std::uint64_t> g_gameplay_state_menu_like{0};
std::atomic<std::uint64_t> g_gameplay_state_known_parent{0};
std::atomic<LONGLONG> g_gameplay_state_first_gameplay_qpc{0};
std::atomic<LONGLONG> g_gameplay_state_last_gameplay_qpc{0};
std::atomic<bool> g_experimental_synthetic_rmb_down{false};
std::atomic<std::uint64_t> g_experimental_synthetic_rmb_down_events{0};
std::atomic<std::uint64_t> g_experimental_synthetic_rmb_up_events{0};
std::atomic<std::uint64_t> g_experimental_key_state_forced{0};
std::atomic<LONGLONG> g_experimental_site9_cached_qpc{0};
std::atomic<std::int64_t> g_experimental_site9_cached_x{0};
std::atomic<std::int64_t> g_experimental_site9_cached_y{0};
std::atomic<HMODULE> g_self_module{nullptr};
std::atomic<std::uintptr_t> g_game_image_base{0};
std::atomic<std::uintptr_t> g_game_image_end{0};
std::atomic<HWND> g_raw_input_sink_hwnd{nullptr};
HANDLE g_external_raw_mapping = nullptr;
HANDLE g_external_raw_stop_event = nullptr;
HANDLE g_external_raw_process = nullptr;
ExternalRawInputShared* g_external_raw_shared = nullptr;
HANDLE g_instance_mutex = nullptr;
// Developer traces. Rows are only buffered on game threads; g_trace_flush_thread writes them.
CsvTrace g_cursor_csv;
CsvTrace g_frames_csv;
std::thread g_trace_flush_thread;
std::atomic<bool> g_stop_trace_flush{false};
std::atomic<bool> g_developer_mode_active{false};
std::atomic<bool> g_developer_mode_configured{false};
std::wstring g_developer_log_directory;
std::wstring g_developer_run_directory;
std::atomic<std::uint64_t> g_frame_index{0};
// Sub-pixel remainder of gain-scaled deltas, so non-integer sensitivity does not drop motion.
// Only touched from the game thread that calls GetCursorPos/SetCursorPos.
std::atomic<double> g_subpixel_x{0.0};
std::atomic<double> g_subpixel_y{0.0};
// In-game Raw Input source. Windows coalesces raw input for background processes to ~125 Hz,
// so the companion cannot see a 1000 Hz mouse at full rate; the game window is foreground and
// can. A WH_GETMESSAGE hook on the game's UI thread reads every WM_INPUT the game pumps.
enum class InputSource : std::uint32_t { Auto = 0, Game = 1, Companion = 2 };
std::atomic<std::uint32_t> g_input_source_setting{static_cast<std::uint32_t>(InputSource::Auto)};
HHOOK g_ingame_hook = nullptr;
std::atomic<std::int64_t> g_ingame_pending_x{0};
std::atomic<std::int64_t> g_ingame_pending_y{0};
std::atomic<LONGLONG> g_ingame_last_qpc{0};
std::atomic<std::int64_t> g_ingame_packets{0};
std::atomic<std::int32_t> g_ingame_polling_hz{0};
std::atomic<std::int32_t> g_ingame_packets_last_second{0};
std::atomic<std::int32_t> g_ingame_max_gap_us{0};
std::atomic<bool> g_last_consume_used_game{false};
std::unique_ptr<MouseRateMeter> g_ingame_meter;
std::mutex g_ingame_meter_mutex;
CsvTrace g_ingame_csv;
std::atomic<bool> g_user_enabled{true};
FpsLock g_fps_lock;
std::atomic<bool> g_runtime_running{false};
std::atomic<bool> g_supported_build{false};
std::mutex g_settings_mutex;
std::wstring g_ini_path;
std::string g_game_version;

struct CursorProtocolStats {
  std::atomic<std::uint64_t> get{0};
  std::atomic<std::uint64_t> set{0};
  std::atomic<std::uint64_t> applied{0};
  std::atomic<std::uint64_t> passthrough{0};
  std::atomic<std::uint64_t> stale{0};
  std::atomic<std::uint64_t> invalid{0};
};

constexpr std::uint32_t kCandidateARva = 0x01DCA6BB;
constexpr std::uint32_t kCandidateADownstreamCallRva = 0x01DCA7F3;
constexpr std::uint32_t kCandidateADownstreamTargetRva = 0x01DCADA0;
constexpr std::uint32_t kCandidateADispatchCallRva = 0x01DCB2E5;
constexpr std::uint32_t kCandidateANotifyCallRva = 0x01DCAEE3;
constexpr std::uint32_t kCandidateASetterCallRva = 0x01DCB632;
constexpr std::uint32_t kCandidateAHandlerCallRva = 0x01DCB3B2;
constexpr std::uint32_t kCandidateBRva = 0x01DDE593;
constexpr std::uint32_t kCandidateBDownstreamCallRva = 0x01DDE62D;
constexpr std::uint32_t kCandidateBDownstreamTargetRva = 0x01DD51A0;
constexpr std::uint32_t kCandidateBDeepCallRva = 0x01DD52B4;
constexpr std::uint32_t kCandidateBBranchCheckCallRva = 0x01DD5526;
constexpr std::uint32_t kCandidateBGlobalCheckCallRva = 0x01DD553B;
constexpr std::uint32_t kCandidateBCameraPairCallRva = 0x01DE22FD;
constexpr std::uint32_t kCandidateBCameraPairTargetRva = 0x01D96750;
constexpr std::uint32_t kCandidateBCameraPairWriteRva = 0x01D9675A;
constexpr std::uint32_t kLiveActionGetCursorParentRva = 0x01DDC394;
constexpr std::uint32_t kCandidateBGetCursorParentRva = 0x01DDE484;
constexpr std::uint32_t kSetCursorCenterReturnRva = 0x01DDC32C;
constexpr std::int32_t kExperimentalVirtualCursorMaxOffsetX = 960;
constexpr std::int32_t kExperimentalVirtualCursorMaxOffsetY = 540;
constexpr std::uint32_t kExperimentalVirtualCursorIdleResetCalls = 240;
constexpr std::uint32_t kExperimentalCenterWarpBurstMin = 3;
constexpr std::uint32_t kExperimentalCenterWarpBurstWindowMs = 80;
constexpr std::uint32_t kExperimentalCenterWarpActiveMs = 120;
constexpr std::uint32_t kExperimentalCenterWarpArmedMs = 2500;
constexpr std::uint32_t kGameplayStateActiveMs = 250;
// Gameplay recenters the cursor every frame; 300 ms covers hitches but lets the menu take over fast.
constexpr std::uint32_t kGameplayStateLatchedActiveMs = 300;
constexpr std::array<std::uint32_t, 6> kCursorProtocolParentBuckets{
    kLiveActionGetCursorParentRva,
    kCandidateBGetCursorParentRva,
    0x01DCA5CA,
    0x01DDE993,
    0x01DE159A,
    0x01DC1F17,
};
constexpr std::size_t kCursorProtocolOtherParentBucket = kCursorProtocolParentBuckets.size();
constexpr std::size_t kCursorProtocolParentBucketCount = kCursorProtocolParentBuckets.size() + 1;
constexpr std::size_t kCursorProtocolButtonMaskCount = 4;
std::array<std::array<CursorProtocolStats, kCursorProtocolButtonMaskCount>, kCursorProtocolParentBucketCount>
    g_cursor_protocol_stats;
constexpr std::array<std::byte, 5> kCandidateAStoreBytes{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x11}, std::byte{0x4D}, std::byte{0xC8}};
constexpr std::array<std::byte, 5> kCandidateADownstreamCallBytes{
    std::byte{0xE8}, std::byte{0xA8}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 5> kCandidateADispatchCallBytes{
    std::byte{0xE8}, std::byte{0x76}, std::byte{0x03}, std::byte{0x01}, std::byte{0x00}};
constexpr std::array<std::byte, 5> kCandidateANotifyCallBytes{
    std::byte{0xE8}, std::byte{0xD8}, std::byte{0x0C}, std::byte{0x01}, std::byte{0x00}};
constexpr std::array<std::byte, 5> kCandidateASetterCallBytes{
    std::byte{0xE8}, std::byte{0xC9}, std::byte{0x0C}, std::byte{0x01}, std::byte{0x00}};
constexpr std::array<std::byte, 6> kCandidateAHandlerCallBytes{
    std::byte{0xFF}, std::byte{0x90}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 6> kCandidateBStoreBytes{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x11}, std::byte{0x4C}, std::byte{0x24}, std::byte{0x78}};
constexpr std::array<std::byte, 5> kCandidateBDownstreamCallBytes{
    std::byte{0xE8}, std::byte{0x6E}, std::byte{0x6B}, std::byte{0xFF}, std::byte{0xFF}};
constexpr std::array<std::byte, 6> kCandidateBDeepCallBytes{
    std::byte{0xFF}, std::byte{0x93}, std::byte{0xF8}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 3> kCandidateBBranchCheckCallBytes{
    std::byte{0xFF}, std::byte{0x50}, std::byte{0x08}};
constexpr std::array<std::byte, 3> kCandidateBGlobalCheckCallBytes{
    std::byte{0xFF}, std::byte{0x50}, std::byte{0x60}};
constexpr std::array<std::byte, 5> kCandidateBCameraPairCallBytes{
    std::byte{0xE8}, std::byte{0x4E}, std::byte{0x44}, std::byte{0xFB}, std::byte{0xFF}};
constexpr std::array<std::byte, 7> kCandidateBCameraPairWriteBytes{
    std::byte{0x48}, std::byte{0x89}, std::byte{0x91}, std::byte{0xA8},
    std::byte{0x07}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 6> kCandidateBSubXBytes{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x5C}, std::byte{0x4C}, std::byte{0x24}, std::byte{0x30}};
constexpr std::array<std::byte, 6> kCandidateBSubYBytes{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x5C}, std::byte{0x54}, std::byte{0x24}, std::byte{0x34}};
constexpr std::array<std::byte, 6> kCandidateASubXBytes{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x5C}, std::byte{0x4C}, std::byte{0x24}, std::byte{0x64}};
constexpr std::array<std::byte, 6> kCandidateASubYBytes{
    std::byte{0xF3}, std::byte{0x0F}, std::byte{0x5C}, std::byte{0x54}, std::byte{0x24}, std::byte{0x68}};
constexpr std::size_t kInternalTraceRingCapacity = 8192;
constexpr std::size_t kArgumentSnapshotFloatCount = 16;

constexpr std::array<const char*, 7> kSegmentLabels{
    "none", "menu", "load_game", "in_car_start", "in_car_mouse", "on_foot_start", "on_foot_mouse"};

const char* segment_label(std::uint32_t id) {
  if (id < kSegmentLabels.size()) {
    return kSegmentLabels[id];
  }
  if (id == 7) {
    return "stop_segment";
  }
  return "unknown";
}

std::uint32_t cursor_button_mask() {
  std::uint32_t mask = 0;
  if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
    mask |= 1U;
  }
  if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
    mask |= 2U;
  }
  return mask;
}

const char* cursor_button_mask_label(std::uint32_t mask) {
  switch (mask & 3U) {
    case 0:
      return "none";
    case 1:
      return "lmb";
    case 2:
      return "rmb";
    default:
      return "both";
  }
}

std::size_t cursor_parent_bucket(std::uintptr_t parent) {
  for (std::size_t i = 0; i < kCursorProtocolParentBuckets.size(); ++i) {
    if (parent == kCursorProtocolParentBuckets[i]) {
      return i;
    }
  }
  return kCursorProtocolOtherParentBucket;
}

std::string cursor_parent_bucket_label(std::size_t bucket) {
  if (bucket < kCursorProtocolParentBuckets.size()) {
    std::ostringstream out;
    out << "0x" << std::hex << kCursorProtocolParentBuckets[bucket] << std::dec;
    return out.str();
  }
  return "other";
}

CursorProtocolStats& cursor_protocol_stats(std::uintptr_t parent, std::uint32_t button_mask) {
  return g_cursor_protocol_stats[cursor_parent_bucket(parent)][button_mask & 3U];
}

void reset_cursor_protocol_stats() {
  for (auto& parent_stats : g_cursor_protocol_stats) {
    for (auto& stats : parent_stats) {
      stats.get.store(0, std::memory_order_release);
      stats.set.store(0, std::memory_order_release);
      stats.applied.store(0, std::memory_order_release);
      stats.passthrough.store(0, std::memory_order_release);
      stats.stale.store(0, std::memory_order_release);
      stats.invalid.store(0, std::memory_order_release);
    }
  }
}

void log_cursor_protocol_stats() {
  for (std::size_t parent_bucket = 0; parent_bucket < g_cursor_protocol_stats.size(); ++parent_bucket) {
    for (std::size_t button_mask = 0; button_mask < g_cursor_protocol_stats[parent_bucket].size(); ++button_mask) {
      const auto& stats = g_cursor_protocol_stats[parent_bucket][button_mask];
      const auto get = stats.get.load(std::memory_order_acquire);
      const auto set = stats.set.load(std::memory_order_acquire);
      const auto applied = stats.applied.load(std::memory_order_acquire);
      const auto passthrough = stats.passthrough.load(std::memory_order_acquire);
      const auto stale = stats.stale.load(std::memory_order_acquire);
      const auto invalid = stats.invalid.load(std::memory_order_acquire);
      if (get == 0 && set == 0 && applied == 0 && passthrough == 0 && stale == 0 && invalid == 0) {
        continue;
      }
      g_logger.info("CursorProtocolStats parent=" + cursor_parent_bucket_label(parent_bucket) +
                    " buttons=" + cursor_button_mask_label(static_cast<std::uint32_t>(button_mask)) +
                    " get=" + std::to_string(get) + " set=" + std::to_string(set) +
                    " applied=" + std::to_string(applied) + " passthrough=" + std::to_string(passthrough) +
                    " stale=" + std::to_string(stale) + " invalid=" + std::to_string(invalid));
    }
  }
}

struct InternalTraceEvent {
  std::uint64_t sequence = 0;
  LONGLONG qpc = 0;
  DWORD thread_id = 0;
  std::uint32_t rva = 0;
  std::uint32_t site = 0;
  std::uint32_t segment_id = 0;
  float delta_x = 0.0F;
  float delta_y = 0.0F;
  float ref_x = 0.0F;
  float ref_y = 0.0F;
  float current_x = 0.0F;
  float current_y = 0.0F;
  float pre_store_x = 0.0F;
  float pre_store_y = 0.0F;
  std::uint32_t memory_ok = 0;
  std::uint32_t arg_memory_ok = 0;
  std::uint32_t stack_arg_memory_ok = 0;
  std::array<float, kArgumentSnapshotFloatCount> rdx_f{};
  std::array<float, kArgumentSnapshotFloatCount> r8_f{};
  std::array<float, kArgumentSnapshotFloatCount> r9_f{};
  std::array<float, kArgumentSnapshotFloatCount> stack20_f{};
  std::uint64_t rax = 0;
  std::uint64_t rbx = 0;
  std::uint64_t rcx = 0;
  std::uint64_t rdx = 0;
  std::uint64_t r8 = 0;
  std::uint64_t r9 = 0;
  std::uint64_t r10 = 0;
  std::uint64_t r11 = 0;
  std::uint64_t r12 = 0;
  std::uint64_t r13 = 0;
  std::uint64_t r14 = 0;
  std::uint64_t r15 = 0;
  std::uint64_t call_target = 0;
  std::uint64_t stack_arg20 = 0;
  std::uint32_t stack_arg28 = 0;
  std::uint64_t rsp = 0;
  std::uint64_t rbp = 0;
};

struct InternalTraceSlot {
  InternalTraceEvent event;
  std::atomic<std::uint64_t> ready{0};
};

struct InternalBreakpoint {
  std::byte* address = nullptr;
  std::uint32_t rva = 0;
  std::uint32_t site = 0;
  std::byte original = std::byte{0};
  std::atomic<bool> installed{false};
};

std::array<InternalTraceSlot, kInternalTraceRingCapacity> g_internal_trace_ring;
std::atomic<std::uint64_t> g_internal_trace_write{0};
std::atomic<std::uint64_t> g_internal_trace_read{0};
std::atomic<std::uint64_t> g_internal_trace_dropped{0};
std::atomic<std::uint64_t> g_internal_trace_hits{0};
std::atomic<std::uint64_t> g_internal_trace_max_rows{0};
InternalBreakpoint g_candidate_a_breakpoint;
InternalBreakpoint g_candidate_a_downstream_breakpoint;
InternalBreakpoint g_candidate_a_dispatch_breakpoint;
InternalBreakpoint g_candidate_a_notify_breakpoint;
InternalBreakpoint g_candidate_a_setter_breakpoint;
InternalBreakpoint g_candidate_a_handler_breakpoint;
InternalBreakpoint g_candidate_b_breakpoint;
InternalBreakpoint g_candidate_b_downstream_breakpoint;
InternalBreakpoint g_candidate_b_deep_breakpoint;
InternalBreakpoint g_candidate_b_branch_check_breakpoint;
InternalBreakpoint g_candidate_b_global_check_breakpoint;
InternalBreakpoint g_candidate_b_camera_pair_breakpoint;
InternalBreakpoint g_candidate_b_camera_pair_write_breakpoint;
void* g_internal_veh_handle = nullptr;
thread_local std::byte* t_internal_trace_single_step = nullptr;

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetClipCursorFn = BOOL(WINAPI*)(LPRECT);
std::atomic<GetProcAddressFn> g_original_get_proc_address{nullptr};
std::atomic<GetCursorPosFn> g_original_get_cursor_pos{nullptr};
std::atomic<SetCursorPosFn> g_original_set_cursor_pos{nullptr};
std::atomic<GetAsyncKeyStateFn> g_original_get_async_key_state{nullptr};
std::atomic<GetKeyStateFn> g_original_get_key_state{nullptr};
std::atomic<ClipCursorFn> g_original_clip_cursor{nullptr};
std::atomic<GetClipCursorFn> g_original_get_clip_cursor{nullptr};

BOOL WINAPI observed_get_cursor_pos(LPPOINT point);
BOOL WINAPI observed_set_cursor_pos(int x, int y);
SHORT WINAPI observed_get_async_key_state(int vkey);
SHORT WINAPI observed_get_key_state(int vkey);

bool equals_ci(const std::string& left, const char* right) {
  return _stricmp(left.c_str(), right) == 0;
}

bool supported_internal_trace_fingerprint(const BuildFingerprint& fp) {
  return fp.is_x64 &&
         equals_ci(fp.sha256, "63E633BB51C35FE35CA98F503B62E7DDB8403ABE4C9D501CFA9162892453846F") &&
         equals_ci(fp.text_sha256, "9749a3f0d50bd84c6b8d2f376604a9a672d950a40324dac2e388aea6e1ab7a43");
}

bool memory_matches(const std::byte* address, const std::byte* expected, std::size_t size) {
  __try {
    return std::memcmp(address, expected, size) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool write_code_byte(std::byte* address, std::byte value, std::byte* old_value) {
  DWORD old_protect = 0;
  if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
    return false;
  }
  if (old_value != nullptr) {
    *old_value = *address;
  }
  *address = value;
  FlushInstructionCache(GetCurrentProcess(), address, 1);
  DWORD ignored = 0;
  VirtualProtect(address, 1, old_protect, &ignored);
  return true;
}

float low_float_from_xmm(const M128A& value) {
  float out = 0.0F;
  const auto low = static_cast<std::uint32_t>(value.Low);
  std::memcpy(&out, &low, sizeof(out));
  return out;
}

std::uint32_t bits_from_float(float value) {
  std::uint32_t out = 0;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

float float_from_bits(std::uint32_t value) {
  float out = 0.0F;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

LONGLONG qpc_ticks_for_ms(std::uint32_t milliseconds) {
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  return (frequency.QuadPart * static_cast<LONGLONG>(milliseconds)) / 1000;
}

bool experimental_recent_center_warp_active() {
  const auto last = g_experimental_last_center_warp_qpc.load(std::memory_order_acquire);
  if (last == 0 || g_experimental_center_warp_burst_count.load(std::memory_order_acquire) < kExperimentalCenterWarpBurstMin) {
    return false;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const auto age = now.QuadPart >= last ? now.QuadPart - last : last - now.QuadPart;
  return age <= qpc_ticks_for_ms(kExperimentalCenterWarpActiveMs);
}

bool experimental_recent_center_warp_armed() {
  const auto last = g_experimental_last_center_warp_qpc.load(std::memory_order_acquire);
  if (last == 0) {
    return false;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const auto age = now.QuadPart >= last ? now.QuadPart - last : last - now.QuadPart;
  return age <= qpc_ticks_for_ms(kExperimentalCenterWarpArmedMs);
}

void experimental_record_center_warp() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const auto previous = g_experimental_last_center_warp_qpc.exchange(now.QuadPart, std::memory_order_acq_rel);
  if (previous != 0 && now.QuadPart >= previous &&
      now.QuadPart - previous <= qpc_ticks_for_ms(kExperimentalCenterWarpBurstWindowMs)) {
    g_experimental_center_warp_burst_count.fetch_add(1, std::memory_order_acq_rel);
  } else {
    g_experimental_center_warp_burst_count.store(1, std::memory_order_release);
  }
}

void gameplay_state_record_set_cursor(int x, int y, std::uintptr_t direct, std::uintptr_t parent) {
  if (!g_observe_gameplay_state.load(std::memory_order_acquire)) {
    return;
  }
  g_gameplay_state_set_total.fetch_add(1, std::memory_order_relaxed);

  const auto center_x = g_experimental_mouse_fix_center_x.load(std::memory_order_acquire);
  const auto center_y = g_experimental_mouse_fix_center_y.load(std::memory_order_acquire);
  const bool near_center = std::abs(x - center_x) <= 2 && std::abs(y - center_y) <= 2;
  const bool known_path = direct == kSetCursorCenterReturnRva || parent == 0x01DC1F17;
  if (known_path) {
    g_gameplay_state_known_parent.fetch_add(1, std::memory_order_relaxed);
  }
  if (near_center) {
    g_gameplay_state_center_like.fetch_add(1, std::memory_order_relaxed);
  }

  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  bool burst_active = false;
  if (near_center && known_path) {
    const auto previous = g_gameplay_state_last_center_warp_qpc.exchange(now.QuadPart, std::memory_order_acq_rel);
    if (previous != 0 && now.QuadPart > previous) {
      // Smoothed interval between recenters, i.e. roughly the gameplay frame time.
      const double interval = static_cast<double>(now.QuadPart - previous);
      const double old_average = g_recenter_interval_ticks.load(std::memory_order_relaxed);
      if (interval < static_cast<double>(qpc_ticks_for_ms(200))) {
        g_recenter_interval_ticks.store(old_average <= 0.0 ? interval : old_average * 0.9 + interval * 0.1,
                                        std::memory_order_relaxed);
      }
    }
    if (previous != 0 && now.QuadPart >= previous &&
        now.QuadPart - previous <= qpc_ticks_for_ms(kExperimentalCenterWarpBurstWindowMs)) {
      const auto burst = g_gameplay_state_center_warp_burst_count.fetch_add(1, std::memory_order_acq_rel) + 1;
      burst_active = burst >= kExperimentalCenterWarpBurstMin;
    } else {
      g_gameplay_state_center_warp_burst_count.store(1, std::memory_order_release);
    }
  }

  if (burst_active) {
    g_gameplay_state_gameplay_like.fetch_add(1, std::memory_order_relaxed);
    LONGLONG expected = 0;
    (void)g_gameplay_state_first_gameplay_qpc.compare_exchange_strong(
        expected, now.QuadPart, std::memory_order_acq_rel, std::memory_order_acquire);
    g_gameplay_state_last_gameplay_qpc.store(now.QuadPart, std::memory_order_release);
  } else {
    g_gameplay_state_menu_like.fetch_add(1, std::memory_order_relaxed);
  }
}

bool gameplay_state_recent_active() {
  const auto last = g_gameplay_state_last_gameplay_qpc.load(std::memory_order_acquire);
  if (last == 0) {
    return false;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const auto age = now.QuadPart >= last ? now.QuadPart - last : last - now.QuadPart;
  const auto mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
  if (mode == 26 || mode == 27) {
    // Gameplay recenters the cursor every frame. Stay "in gameplay" for a few frames of silence
    // (hitches), but hand the cursor back to the menu within a few frames instead of a fixed delay.
    const double average = g_recenter_interval_ticks.load(std::memory_order_relaxed);
    const auto window = average > 0.0 ? static_cast<LONGLONG>(average * 4.0) : qpc_ticks_for_ms(kGameplayStateLatchedActiveMs);
    return age <= std::clamp(window, qpc_ticks_for_ms(20), qpc_ticks_for_ms(kGameplayStateLatchedActiveMs));
  }
  const auto active_ms = (mode == 23 || mode == 24 || mode == 25) ? kGameplayStateLatchedActiveMs : kGameplayStateActiveMs;
  return age <= qpc_ticks_for_ms(active_ms);
}

// The in-game source is used while it delivers input. It can go quiet if the game drops its raw
// input registration; then the companion (always running) takes over.
bool ingame_source_live(LONGLONG companion_last_qpc) {
  const auto setting = static_cast<InputSource>(g_input_source_setting.load(std::memory_order_relaxed));
  if (setting == InputSource::Companion || g_ingame_hook == nullptr) {
    return false;
  }
  if (setting == InputSource::Game) {
    return true;
  }
  const auto ingame_last = g_ingame_last_qpc.load(std::memory_order_acquire);
  if (ingame_last == 0) {
    return false;
  }
  // Live when it saw the latest motion too (allowing for the game pumping once per frame).
  return ingame_last + qpc_ticks_for_ms(100) >= companion_last_qpc;
}

std::pair<std::int64_t, std::int64_t> consume_protocol_delta() {
  const auto mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
  if ((mode == 26 || mode == 27) && g_external_raw_shared != nullptr) {
    // Drain both sources every time so switching between them never replays old motion.
    const std::pair<std::int64_t, std::int64_t> companion{InterlockedExchange64(&g_external_raw_shared->pending_x, 0),
                                                          InterlockedExchange64(&g_external_raw_shared->pending_y, 0)};
    const std::pair<std::int64_t, std::int64_t> ingame{g_ingame_pending_x.exchange(0, std::memory_order_acq_rel),
                                                       g_ingame_pending_y.exchange(0, std::memory_order_acq_rel)};
    const bool use_game = ingame_source_live(InterlockedCompareExchange64(&g_external_raw_shared->last_qpc, 0, 0));
    g_last_consume_used_game.store(use_game, std::memory_order_relaxed);
    return use_game ? ingame : companion;
  }
  return g_queue ? g_queue->consume_pending_delta() : std::pair<std::int64_t, std::int64_t>{0, 0};
}

void ingame_raw_input(HRAWINPUT input) {
  alignas(8) std::array<std::byte, 256> buffer{};
  UINT size = static_cast<UINT>(buffer.size());
  if (GetRawInputData(input, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
      size < sizeof(RAWINPUTHEADER)) {
    return;
  }
  const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
  if (raw->header.dwType != RIM_TYPEMOUSE || (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
    return;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  g_ingame_pending_x.fetch_add(raw->data.mouse.lLastX, std::memory_order_acq_rel);
  g_ingame_pending_y.fetch_add(raw->data.mouse.lLastY, std::memory_order_acq_rel);
  g_ingame_last_qpc.store(now.QuadPart, std::memory_order_release);
  g_ingame_packets.fetch_add(1, std::memory_order_relaxed);
  MouseRateMeter::Snapshot snapshot{};
  bool snapshot_ready = false;
  {
    // Fed from the sink thread and from the game's UI thread (when the game holds the mouse).
    std::lock_guard lock(g_ingame_meter_mutex);
    snapshot_ready = g_ingame_meter && g_ingame_meter->record(now.QuadPart, snapshot);
  }
  if (snapshot_ready) {
    g_ingame_polling_hz.store(snapshot.polling_hz, std::memory_order_relaxed);
    g_ingame_packets_last_second.store(snapshot.packets_last_second, std::memory_order_relaxed);
    g_ingame_max_gap_us.store(snapshot.max_gap_us_last_second, std::memory_order_relaxed);
  }
  if (g_ingame_csv.enabled()) {
    char row[96]{};
    const int length = std::snprintf(row, sizeof(row), "%lld,%ld,%ld,%u", static_cast<long long>(now.QuadPart),
                                     raw->data.mouse.lLastX, raw->data.mouse.lLastY, raw->data.mouse.usButtonFlags);
    if (length > 0) {
      g_ingame_csv.write_row(row, static_cast<std::size_t>(length));
    }
  }
}

LRESULT CALLBACK ingame_message_hook(int code, WPARAM wparam, LPARAM lparam) {
  // PM_NOREMOVE peeks would see the same message twice; count it when it is removed.
  if (code == HC_ACTION && wparam == PM_REMOVE) {
    const auto* message = reinterpret_cast<const MSG*>(lparam);
    if (message != nullptr && message->message == WM_INPUT) {
      ingame_raw_input(reinterpret_cast<HRAWINPUT>(message->lParam));
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

struct GameWindowSearch {
  DWORD process_id = 0;
  HWND window = nullptr;
};

BOOL CALLBACK find_game_window(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<GameWindowSearch*>(parameter);
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id != search->process_id || !IsWindowVisible(window)) {
    return TRUE;
  }
  wchar_t class_name[64]{};
  GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
  if (std::wcscmp(class_name, L"UnrealWindow") == 0) {
    search->window = window;
    return FALSE;
  }
  return TRUE;
}

bool install_ingame_input_hook() {
  if (g_ingame_hook != nullptr) {
    return true;
  }
  if (static_cast<InputSource>(g_input_source_setting.load()) == InputSource::Companion) {
    return false;
  }
  GameWindowSearch search{GetCurrentProcessId(), nullptr};
  EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
  if (search.window == nullptr) {
    return false;  // Window not created yet; the hook thread retries.
  }
  const DWORD thread_id = GetWindowThreadProcessId(search.window, nullptr);
  g_ingame_hook = SetWindowsHookExW(WH_GETMESSAGE, ingame_message_hook, g_self_module.load(), thread_id);
  if (g_ingame_hook == nullptr) {
    g_logger.warn("In-game Raw Input: SetWindowsHookEx failed with " + std::to_string(GetLastError()));
    return false;
  }
  g_logger.info("In-game Raw Input hook installed on UI thread " + std::to_string(thread_id));
  return true;
}

// Raw input registration is per process: one target window per device type. The game only
// registers the mouse for short moments (and removes it afterwards), so between those moments a
// hidden window of ours inside the game process holds the registration. Both stay in the
// foreground process, so Windows delivers the full report rate either way.
constexpr UINT kSinkRegisterMessage = WM_APP + 0x51;
std::thread g_sink_thread;
std::atomic<HWND> g_sink_window{nullptr};
std::atomic<bool> g_sink_registered{false};
std::atomic<std::uint64_t> g_sink_registrations{0};

bool game_holds_mouse_registration() {
  UINT count = 0;
  if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) && count == 0) {
    return false;
  }
  std::array<RAWINPUTDEVICE, 16> devices{};
  count = static_cast<UINT>(devices.size());
  const UINT found = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
  if (found == static_cast<UINT>(-1)) {
    return false;
  }
  const HWND sink = g_sink_window.load(std::memory_order_acquire);
  for (UINT i = 0; i < found; ++i) {
    if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02 && devices[i].hwndTarget != sink) {
      return true;
    }
  }
  return false;
}

// Runs on the sink thread.
void register_sink() {
  const HWND sink = g_sink_window.load(std::memory_order_acquire);
  if (sink == nullptr || game_holds_mouse_registration()) {
    g_sink_registered.store(false, std::memory_order_release);
    return;
  }
  RAWINPUTDEVICE device{};
  device.usUsagePage = 0x01;
  device.usUsage = 0x02;
  device.dwFlags = RIDEV_INPUTSINK;
  device.hwndTarget = sink;
  const bool ok = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
  g_sink_registered.store(ok, std::memory_order_release);
  if (ok) {
    g_sink_registrations.fetch_add(1, std::memory_order_relaxed);
  }
}

LRESULT CALLBACK sink_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_INPUT) {
    ingame_raw_input(reinterpret_cast<HRAWINPUT>(lparam));
  } else if (message == kSinkRegisterMessage) {
    register_sink();
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void sink_loop() {
  constexpr wchar_t kClass[] = L"GTASADE.PCFix.InProcessSink";
  WNDCLASSW window_class{};
  window_class.hInstance = g_self_module.load();
  window_class.lpfnWndProc = sink_window_proc;
  window_class.lpszClassName = kClass;
  RegisterClassW(&window_class);
  HWND window = CreateWindowExW(0, kClass, kClass, 0, 0, 0, 0, 0, nullptr, nullptr, g_self_module.load(), nullptr);
  if (window == nullptr) {
    g_logger.warn("In-process Raw Input sink window could not be created");
    return;
  }
  g_sink_window.store(window, std::memory_order_release);
  register_sink();
  g_logger.info(std::string("In-process Raw Input sink ready; registered=") +
                (g_sink_registered.load() ? "true" : "false (game holds the mouse)"));
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    DispatchMessageW(&message);
  }
  g_sink_window.store(nullptr, std::memory_order_release);
  DestroyWindow(window);
}

void stop_sink() {
  if (const HWND window = g_sink_window.load(std::memory_order_acquire)) {
    PostMessageW(window, WM_QUIT, 0, 0);
  }
  if (g_sink_thread.joinable()) {
    g_sink_thread.join();
  }
}

// Called after the game's own RegisterRawInputDevices. When it removes the mouse, the process has
// no mouse registration left, so the sink takes it back.
void on_game_raw_input_registration(PCRAWINPUTDEVICE devices, UINT count) {
  const HWND sink = g_sink_window.load(std::memory_order_acquire);
  if (sink == nullptr || devices == nullptr) {
    return;
  }
  for (UINT i = 0; i < count; ++i) {
    if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02) {
      if ((devices[i].dwFlags & RIDEV_REMOVE) != 0) {
        PostMessageW(sink, kSinkRegisterMessage, 0, 0);
      } else {
        g_sink_registered.store(false, std::memory_order_release);  // Replaced by the game's window.
      }
    }
  }
}

std::thread g_ingame_hook_thread;
std::atomic<bool> g_stop_ingame_hook{false};

// Thread hooks are released when the thread that installed them exits, so this thread owns the
// hook for the whole session: it waits for the game window, installs, then idles until stop.
void ingame_hook_loop() {
  int attempts = 0;
  while (!g_stop_ingame_hook.load(std::memory_order_acquire)) {
    if (g_ingame_hook == nullptr && attempts < 120) {
      ++attempts;
      if (!install_ingame_input_hook() && attempts == 120) {
        g_logger.warn("In-game Raw Input: giving up, the companion stays the input source");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(g_ingame_hook == nullptr ? 1000 : 200));
  }
  if (g_ingame_hook != nullptr) {
    UnhookWindowsHookEx(g_ingame_hook);
    g_ingame_hook = nullptr;
  }
}

// One row per hooked cursor call in developer mode. action: applied, stale, passthrough, disabled, set.
void trace_cursor(const char* kind,
                  const char* action,
                  std::uintptr_t parent,
                  std::uint32_t buttons,
                  std::int64_t dx,
                  std::int64_t dy,
                  LONG out_x,
                  LONG out_y) {
  if (!g_cursor_csv.enabled()) {
    return;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const LONGLONG companion_qpc =
      g_external_raw_shared != nullptr ? InterlockedCompareExchange64(&g_external_raw_shared->last_qpc, 0, 0) : 0;
  char row[320]{};
  const int length = std::snprintf(
      row, sizeof(row), "%lld,%llu,%s,%s,0x%llx,%u,%d,%lld,%lld,%d,%d,%ld,%ld,%.4f,%.4f,%lld",
      static_cast<long long>(now.QuadPart), static_cast<unsigned long long>(g_frame_index.load(std::memory_order_relaxed)),
      kind, action, static_cast<unsigned long long>(parent), buttons, gameplay_state_recent_active() ? 1 : 0,
      static_cast<long long>(dx), static_cast<long long>(dy), g_experimental_virtual_cursor_x.load(std::memory_order_relaxed),
      g_experimental_virtual_cursor_y.load(std::memory_order_relaxed), out_x, out_y,
      g_subpixel_x.load(std::memory_order_relaxed), g_subpixel_y.load(std::memory_order_relaxed),
      static_cast<long long>(companion_qpc));
  if (length > 0) {
    g_cursor_csv.write_row(row, static_cast<std::size_t>(std::min<int>(length, sizeof(row) - 1)));
  }
}

void stop_external_raw_input_companion() {
  if (g_external_raw_stop_event != nullptr) {
    SetEvent(g_external_raw_stop_event);
  }
  if (g_external_raw_process != nullptr) {
    (void)WaitForSingleObject(g_external_raw_process, 1000);
    CloseHandle(g_external_raw_process);
  }
  if (g_external_raw_shared != nullptr) {
    UnmapViewOfFile(g_external_raw_shared);
  }
  if (g_external_raw_stop_event != nullptr) {
    CloseHandle(g_external_raw_stop_event);
  }
  if (g_external_raw_mapping != nullptr) {
    CloseHandle(g_external_raw_mapping);
  }
  g_external_raw_process = nullptr;
  g_external_raw_stop_event = nullptr;
  g_external_raw_mapping = nullptr;
  g_external_raw_shared = nullptr;
}

bool start_external_raw_input_companion() {
  const auto process_id = GetCurrentProcessId();
  const std::wstring suffix = std::to_wstring(process_id);
  const std::wstring mapping_name = L"Local\\GTASADE.PCFix.ExternalRawInput." + suffix;
  const std::wstring stop_event_name = L"Local\\GTASADE.PCFix.ExternalRawInputStop." + suffix;
  g_external_raw_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                               sizeof(ExternalRawInputShared), mapping_name.c_str());
  if (g_external_raw_mapping == nullptr) {
    return false;
  }
  g_external_raw_shared = static_cast<ExternalRawInputShared*>(
      MapViewOfFile(g_external_raw_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExternalRawInputShared)));
  if (g_external_raw_shared == nullptr) {
    stop_external_raw_input_companion();
    return false;
  }
  g_external_raw_shared->magic = kExternalRawInputMagic;
  g_external_raw_shared->version = kExternalRawInputVersion;
  InterlockedExchange64(&g_external_raw_shared->pending_x, 0);
  InterlockedExchange64(&g_external_raw_shared->pending_y, 0);
  InterlockedExchange64(&g_external_raw_shared->last_qpc, 0);
  InterlockedExchange64(&g_external_raw_shared->packet_count, 0);
  g_external_raw_stop_event = CreateEventW(nullptr, TRUE, FALSE, stop_event_name.c_str());
  if (g_external_raw_stop_event == nullptr) {
    stop_external_raw_input_companion();
    return false;
  }

  std::array<wchar_t, MAX_PATH> module_path{};
  const DWORD length = GetModuleFileNameW(g_self_module.load(std::memory_order_acquire), module_path.data(), MAX_PATH);
  if (length == 0 || length >= module_path.size()) {
    stop_external_raw_input_companion();
    return false;
  }
  const auto companion_path = std::filesystem::path(module_path.data()).parent_path() / L"GTASADE.PCFix.RawInputHelper.exe";
  if (!std::filesystem::exists(companion_path)) {
    stop_external_raw_input_companion();
    return false;
  }
  std::wstring command = L"\"" + companion_path.wstring() + L"\" --mapping \"" + mapping_name +
                         L"\" --stop-event \"" + stop_event_name + L"\"";
  if (g_developer_mode_active.load(std::memory_order_acquire) && !g_developer_run_directory.empty()) {
    command += L" --log \"" + (std::filesystem::path(g_developer_run_directory) / L"companion.csv").wstring() + L"\"";
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      companion_path.parent_path().c_str(), &startup, &process)) {
    stop_external_raw_input_companion();
    return false;
  }
  CloseHandle(process.hThread);
  g_external_raw_process = process.hProcess;
  return true;
}

void experimental_mutate_raw_mouse(RAWMOUSE& mouse) {
  const auto mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
  if (mode != 17 || !g_experimental_rinput_protocol_active.load(std::memory_order_acquire)) {
    return;
  }

  const bool physical_rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  const bool physical_lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  const bool should_hold = experimental_recent_center_warp_armed() && !physical_rmb && !physical_lmb;
  const bool was_down = g_experimental_synthetic_rmb_down.load(std::memory_order_acquire);

  if (should_hold && !was_down) {
    mouse.usButtonFlags = static_cast<USHORT>((mouse.usButtonFlags & ~RI_MOUSE_RIGHT_BUTTON_UP) | RI_MOUSE_RIGHT_BUTTON_DOWN);
    g_experimental_synthetic_rmb_down.store(true, std::memory_order_release);
    g_experimental_synthetic_rmb_down_events.fetch_add(1, std::memory_order_relaxed);
  } else if (!should_hold && was_down) {
    mouse.usButtonFlags = static_cast<USHORT>((mouse.usButtonFlags & ~RI_MOUSE_RIGHT_BUTTON_DOWN) | RI_MOUSE_RIGHT_BUTTON_UP);
    g_experimental_synthetic_rmb_down.store(false, std::memory_order_release);
    g_experimental_synthetic_rmb_up_events.fetch_add(1, std::memory_order_relaxed);
  }
}

void set_low_float(M128A& value, float replacement) {
  const auto bits = static_cast<std::uint64_t>(bits_from_float(replacement));
  value.Low = (value.Low & 0xFFFFFFFF00000000ULL) | bits;
}

float float_from_u32(std::uint32_t value) {
  float out = 0.0F;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

bool read_float_at(std::uint64_t address, float& out) {
  __try {
    std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = 0.0F;
    return false;
  }
}

bool write_float_at(std::uint64_t address, float value) {
  __try {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool read_u64_at(std::uint64_t address, std::uint64_t& out) {
  __try {
    std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = 0;
    return false;
  }
}

bool read_u32_at(std::uint64_t address, std::uint32_t& out) {
  __try {
    std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = 0;
    return false;
  }
}

void append_float_array(std::ostringstream& row, const std::array<float, kArgumentSnapshotFloatCount>& values) {
  for (const auto value : values) {
    row << ',' << value;
  }
}

std::string argument_snapshot_header(const char* prefix) {
  std::ostringstream out;
  for (std::size_t i = 0; i < kArgumentSnapshotFloatCount; ++i) {
    out << ',' << prefix << i;
  }
  return out.str();
}

void write_marker(std::uint32_t segment_id, const char* label) {
  LARGE_INTEGER qpc{};
  QueryPerformanceCounter(&qpc);
  SYSTEMTIME st{};
  GetSystemTime(&st);
  char timestamp[64]{};
  std::snprintf(timestamp,
                sizeof(timestamp),
                "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                st.wYear,
                st.wMonth,
                st.wDay,
                st.wHour,
                st.wMinute,
                st.wSecond,
                st.wMilliseconds);
  g_current_segment_id.store(segment_id, std::memory_order_release);
  std::ostringstream row;
  row << qpc.QuadPart << ',' << timestamp << ',' << GetCurrentThreadId() << ',' << segment_id << ',' << label;
  g_marker_csv.write_row(row.str());
  g_logger.info(std::string("Marker segment_id=") + std::to_string(segment_id) + " label=" + label +
                " qpc=" + std::to_string(qpc.QuadPart));
}

void marker_loop() {
  struct Hotkey {
    int vk = 0;
    bool ctrl = false;
    bool shift = false;
    std::uint32_t segment_id = 0;
    const char* label = "";
    bool was_down = false;
  };
  std::array<Hotkey, 7> hotkeys{{
      {VK_F6, false, false, 1, "menu", false},
      {VK_F7, false, false, 2, "load_game", false},
      {VK_F8, true, false, 3, "in_car_start", false},
      {VK_F8, false, false, 4, "in_car_mouse", false},
      {VK_F9, true, false, 5, "on_foot_start", false},
      {VK_F9, false, false, 6, "on_foot_mouse", false},
      {VK_F9, false, true, 7, "stop_segment", false},
  }};

  while (!g_stop_markers.load(std::memory_order_acquire)) {
    const bool ctrl_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    for (auto& hotkey : hotkeys) {
      const bool modifier_match = ctrl_down == hotkey.ctrl && shift_down == hotkey.shift;
      const bool down = modifier_match && ((GetAsyncKeyState(hotkey.vk) & 0x8000) != 0);
      if (down && !hotkey.was_down) {
        write_marker(hotkey.segment_id, hotkey.label);
      }
      hotkey.was_down = down;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

InternalBreakpoint* breakpoint_for_address(std::byte* address) {
  if (address == g_candidate_a_breakpoint.address) {
    return &g_candidate_a_breakpoint;
  }
  if (address == g_candidate_a_downstream_breakpoint.address) {
    return &g_candidate_a_downstream_breakpoint;
  }
  if (address == g_candidate_a_dispatch_breakpoint.address) {
    return &g_candidate_a_dispatch_breakpoint;
  }
  if (address == g_candidate_a_notify_breakpoint.address) {
    return &g_candidate_a_notify_breakpoint;
  }
  if (address == g_candidate_a_setter_breakpoint.address) {
    return &g_candidate_a_setter_breakpoint;
  }
  if (address == g_candidate_a_handler_breakpoint.address) {
    return &g_candidate_a_handler_breakpoint;
  }
  if (address == g_candidate_b_breakpoint.address) {
    return &g_candidate_b_breakpoint;
  }
  if (address == g_candidate_b_downstream_breakpoint.address) {
    return &g_candidate_b_downstream_breakpoint;
  }
  if (address == g_candidate_b_deep_breakpoint.address) {
    return &g_candidate_b_deep_breakpoint;
  }
  if (address == g_candidate_b_branch_check_breakpoint.address) {
    return &g_candidate_b_branch_check_breakpoint;
  }
  if (address == g_candidate_b_global_check_breakpoint.address) {
    return &g_candidate_b_global_check_breakpoint;
  }
  if (address == g_candidate_b_camera_pair_breakpoint.address) {
    return &g_candidate_b_camera_pair_breakpoint;
  }
  if (address == g_candidate_b_camera_pair_write_breakpoint.address) {
    return &g_candidate_b_camera_pair_write_breakpoint;
  }
  return nullptr;
}

void record_internal_trace(const CONTEXT& context, const InternalBreakpoint& breakpoint) {
  const auto max_rows = g_internal_trace_max_rows.load(std::memory_order_acquire);
  const auto sequence = g_internal_trace_write.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (max_rows != 0 && sequence > max_rows) {
    g_internal_trace_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const auto read = g_internal_trace_read.load(std::memory_order_acquire);
  if (sequence - read > kInternalTraceRingCapacity) {
    g_internal_trace_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  LARGE_INTEGER qpc{};
  QueryPerformanceCounter(&qpc);
  auto& slot = g_internal_trace_ring[(sequence - 1) % kInternalTraceRingCapacity];
  const auto base = g_game_image_base.load(std::memory_order_acquire);
  slot.event.sequence = sequence;
  slot.event.qpc = qpc.QuadPart;
  slot.event.thread_id = GetCurrentThreadId();
  slot.event.rva = base == 0 ? breakpoint.rva : static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(breakpoint.address) - base);
  slot.event.site = breakpoint.site;
  slot.event.segment_id = g_current_segment_id.load(std::memory_order_acquire);
  slot.event.arg_memory_ok = 0U;
  slot.event.stack_arg_memory_ok = 0U;
  slot.event.rdx_f = {};
  slot.event.r8_f = {};
  slot.event.r9_f = {};
  slot.event.stack20_f = {};
  slot.event.stack_arg20 = 0;
  slot.event.stack_arg28 = 0;
  slot.event.delta_x = low_float_from_xmm(context.Xmm1);
  slot.event.delta_y = low_float_from_xmm(context.Xmm2);
  bool memory_ok = true;
  if (breakpoint.site >= 3 && breakpoint.site <= 6) {
    memory_ok = read_u64_at(context.Rbp - 0x60, slot.event.stack_arg20) && memory_ok;
    memory_ok = read_u32_at(context.Rbp - 0x78, slot.event.stack_arg28) && memory_ok;
    for (std::size_t i = 0; i < slot.event.stack20_f.size(); ++i) {
      memory_ok =
          read_float_at(slot.event.stack_arg20 + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.stack20_f[i]) &&
          memory_ok;
    }
    slot.event.current_x = slot.event.stack20_f[6];
    slot.event.current_y = slot.event.stack20_f[7];
    slot.event.ref_x = slot.event.stack20_f[8];
    slot.event.ref_y = slot.event.stack20_f[9];
    slot.event.delta_x = slot.event.stack20_f[10];
    slot.event.delta_y = slot.event.stack20_f[11];
    slot.event.pre_store_x = slot.event.delta_x;
    slot.event.pre_store_y = slot.event.delta_y;
  } else if (breakpoint.site == 7) {
    memory_ok = read_float_at(context.Rsp + 0x30, slot.event.ref_x) && memory_ok;
    memory_ok = read_float_at(context.Rsp + 0x34, slot.event.ref_y) && memory_ok;
    memory_ok = read_float_at(context.Rsp + 0x78, slot.event.pre_store_x) && memory_ok;
    memory_ok = read_float_at(context.Rsp + 0x7C, slot.event.pre_store_y) && memory_ok;
    slot.event.stack_arg20 = context.Rsp + 0x50;
    slot.event.stack_arg28 = 0;
    for (std::size_t i = 0; i < slot.event.stack20_f.size(); ++i) {
      read_float_at(slot.event.stack_arg20 + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.stack20_f[i]);
    }
  } else if (breakpoint.site == 8 || (breakpoint.site >= 9 && breakpoint.site <= 11)) {
    slot.event.stack_arg20 = breakpoint.site == 8 ? context.Rdx : context.R13;
    slot.event.stack_arg28 = breakpoint.site == 8 ? (context.R8 & 0xFFU) : (context.R15 & 0xFFFFFFFFU);
    bool stack_arg_memory_ok = true;
    for (std::size_t i = 0; i < slot.event.stack20_f.size(); ++i) {
      stack_arg_memory_ok =
          read_float_at(slot.event.stack_arg20 + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.stack20_f[i]) &&
          stack_arg_memory_ok;
    }
    slot.event.current_x = slot.event.stack20_f[6];
    slot.event.current_y = slot.event.stack20_f[7];
    slot.event.ref_x = slot.event.stack20_f[8];
    slot.event.ref_y = slot.event.stack20_f[9];
    slot.event.delta_x = slot.event.stack20_f[10];
    slot.event.delta_y = slot.event.stack20_f[11];
    slot.event.pre_store_x = slot.event.delta_x;
    slot.event.pre_store_y = slot.event.delta_y;
    memory_ok = stack_arg_memory_ok && memory_ok;
  } else if (breakpoint.site == 12 || breakpoint.site == 13) {
    slot.event.stack_arg20 = breakpoint.site == 12 ? context.R13 : context.Rcx;
    slot.event.stack_arg28 = 0;
    slot.event.delta_x = float_from_u32(static_cast<std::uint32_t>(context.Rdx & 0xFFFFFFFFULL));
    slot.event.delta_y = float_from_u32(static_cast<std::uint32_t>((context.Rdx >> 32U) & 0xFFFFFFFFULL));
    slot.event.current_x = slot.event.delta_x;
    slot.event.current_y = slot.event.delta_y;
    slot.event.pre_store_x = slot.event.delta_x;
    slot.event.pre_store_y = slot.event.delta_y;
  } else {
    memory_ok = read_float_at(context.Rsp + 0x64, slot.event.ref_x) && memory_ok;
    memory_ok = read_float_at(context.Rsp + 0x68, slot.event.ref_y) && memory_ok;
    memory_ok = read_float_at(context.Rbp - 0x38, slot.event.pre_store_x) && memory_ok;
    memory_ok = read_float_at(context.Rbp - 0x34, slot.event.pre_store_y) && memory_ok;
  }
  if (breakpoint.site == 2) {
    slot.event.delta_x = slot.event.pre_store_x;
    slot.event.delta_y = slot.event.pre_store_y;
  }
  if (breakpoint.site < 3 || breakpoint.site == 7) {
    slot.event.current_x = slot.event.ref_x + slot.event.delta_x;
    slot.event.current_y = slot.event.ref_y + slot.event.delta_y;
  }
  if (breakpoint.site == 2) {
    slot.event.call_target = base == 0 ? 0 : base + kCandidateADownstreamTargetRva;
  } else if (breakpoint.site == 8) {
    slot.event.call_target = base == 0 ? 0 : base + kCandidateBDownstreamTargetRva;
  } else if (breakpoint.site == 9) {
    read_u64_at(context.Rbx + 0xF8, slot.event.call_target);
  } else if (breakpoint.site == 10) {
    read_u64_at(context.Rax + 0x08, slot.event.call_target);
  } else if (breakpoint.site == 11) {
    read_u64_at(context.Rax + 0x60, slot.event.call_target);
  } else if (breakpoint.site == 12) {
    slot.event.call_target = base == 0 ? 0 : base + kCandidateBCameraPairTargetRva;
  } else if (breakpoint.site == 13) {
    slot.event.call_target = 0;
  } else {
    slot.event.call_target = 0;
  }
  slot.event.memory_ok = memory_ok ? 1U : 0U;
  slot.event.rax = context.Rax;
  slot.event.rbx = context.Rbx;
  slot.event.rcx = context.Rcx;
  slot.event.rdx = context.Rdx;
  slot.event.r8 = context.R8;
  slot.event.r9 = context.R9;
  slot.event.r10 = context.R10;
  slot.event.r11 = context.R11;
  slot.event.r12 = context.R12;
  slot.event.r13 = context.R13;
  slot.event.r14 = context.R14;
  slot.event.r15 = context.R15;
  slot.event.rsp = context.Rsp;
  slot.event.rbp = context.Rbp;
  if (breakpoint.site == 2) {
    bool arg_memory_ok = true;
    bool stack_arg_memory_ok = true;
    for (std::size_t i = 0; i < slot.event.rdx_f.size(); ++i) {
      arg_memory_ok = read_float_at(context.Rdx + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.rdx_f[i]) &&
                      arg_memory_ok;
      arg_memory_ok = read_float_at(context.R8 + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.r8_f[i]) &&
                      arg_memory_ok;
      arg_memory_ok = read_float_at(context.R9 + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.r9_f[i]) &&
                      arg_memory_ok;
    }
    slot.event.arg_memory_ok = arg_memory_ok ? 1U : 0U;
    stack_arg_memory_ok = read_u64_at(context.Rsp + 0x20, slot.event.stack_arg20) && stack_arg_memory_ok;
    stack_arg_memory_ok = read_u32_at(context.Rsp + 0x28, slot.event.stack_arg28) && stack_arg_memory_ok;
    for (std::size_t i = 0; i < slot.event.stack20_f.size(); ++i) {
      stack_arg_memory_ok =
          read_float_at(slot.event.stack_arg20 + static_cast<std::uint64_t>(i * sizeof(float)), slot.event.stack20_f[i]) &&
          stack_arg_memory_ok;
    }
    slot.event.stack_arg_memory_ok = stack_arg_memory_ok ? 1U : 0U;
  } else if (breakpoint.site >= 3) {
    slot.event.arg_memory_ok = 0U;
    slot.event.stack_arg_memory_ok = memory_ok ? 1U : 0U;
  } else {
  }
  slot.ready.store(sequence, std::memory_order_release);
  g_internal_trace_hits.fetch_add(1, std::memory_order_relaxed);
}

void apply_experimental_mouse_fix(CONTEXT& context, const InternalBreakpoint& breakpoint) {
  if (!g_experimental_mouse_fix.load(std::memory_order_acquire)) {
    return;
  }

  const auto gain_x = float_from_bits(g_experimental_mouse_fix_gain_x_bits.load(std::memory_order_acquire));
  const auto gain_y = float_from_bits(g_experimental_mouse_fix_gain_y_bits.load(std::memory_order_acquire));
  const auto mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);

  if (mode == 7 || mode == 8 || mode == 9) {
    if (breakpoint.site != 7) {
      return;
    }
    const auto delta = g_queue ? g_queue->consume_pending_delta() : std::pair<std::int64_t, std::int64_t>{0, 0};
    if (delta.first == 0 && delta.second == 0) {
      g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto replacement_x = static_cast<float>(delta.first) * gain_x;
    const auto replacement_y = static_cast<float>(delta.second) * gain_y;
    if (!std::isfinite(replacement_x) || !std::isfinite(replacement_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    set_low_float(context.Xmm1, replacement_x);
    set_low_float(context.Xmm2, replacement_y);
    g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (mode == 3) {
    if (breakpoint.site != 9) {
      return;
    }
    const auto stack_arg28 = static_cast<std::uint32_t>(context.R15 & 0xFFFFFFFFULL);
    if (stack_arg28 != 0x200U || context.R13 == 0) {
      g_experimental_patch_passthrough.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    float current_x = 0.0F;
    float current_y = 0.0F;
    float ref_x = 0.0F;
    float ref_y = 0.0F;
    float delta_x = 0.0F;
    float delta_y = 0.0F;
    const auto payload = context.R13;
    if (!read_float_at(payload + 0x18, current_x) || !read_float_at(payload + 0x1C, current_y) ||
        !read_float_at(payload + 0x20, ref_x) || !read_float_at(payload + 0x24, ref_y) ||
        !read_float_at(payload + 0x28, delta_x) || !read_float_at(payload + 0x2C, delta_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const auto replacement_x = delta_x * gain_x;
    const auto replacement_y = delta_y * gain_y;
    const auto replacement_current_x = ref_x + replacement_x;
    const auto replacement_current_y = ref_y + replacement_y;
    if (!std::isfinite(replacement_x) || !std::isfinite(replacement_y) ||
        !std::isfinite(replacement_current_x) || !std::isfinite(replacement_current_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (!write_float_at(payload + 0x18, replacement_current_x) ||
        !write_float_at(payload + 0x1C, replacement_current_y) ||
        !write_float_at(payload + 0x28, replacement_x) ||
        !write_float_at(payload + 0x2C, replacement_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (mode == 19) {
    if (breakpoint.site != 9) {
      return;
    }
    const auto stack_arg28 = static_cast<std::uint32_t>(context.R15 & 0xFFFFFFFFULL);
    if (stack_arg28 != 0x1U || context.R13 == 0) {
      g_experimental_patch_passthrough.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    float ref_x = 0.0F;
    float ref_y = 0.0F;
    const auto payload = context.R13;
    if (!read_float_at(payload + 0x20, ref_x) || !read_float_at(payload + 0x24, ref_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const auto delta = g_queue ? g_queue->consume_pending_delta() : std::pair<std::int64_t, std::int64_t>{0, 0};
    if (delta.first == 0 && delta.second == 0) {
      g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const auto replacement_x = static_cast<float>(delta.first) * gain_x;
    const auto replacement_y = static_cast<float>(delta.second) * gain_y;
    const auto replacement_current_x = ref_x + replacement_x;
    const auto replacement_current_y = ref_y + replacement_y;
    if (!std::isfinite(replacement_x) || !std::isfinite(replacement_y) ||
        !std::isfinite(replacement_current_x) || !std::isfinite(replacement_current_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (!write_float_at(payload + 0x18, replacement_current_x) ||
        !write_float_at(payload + 0x1C, replacement_current_y) ||
        !write_float_at(payload + 0x28, replacement_x) ||
        !write_float_at(payload + 0x2C, replacement_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (mode == 20) {
    if (breakpoint.site != 9) {
      return;
    }
    const auto stack_arg28 = static_cast<std::uint32_t>(context.R15 & 0xFFFFFFFFULL);
    if ((stack_arg28 != 0x1U && stack_arg28 != 0x200U) || context.R13 == 0) {
      g_experimental_patch_passthrough.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    float ref_x = 0.0F;
    float ref_y = 0.0F;
    const auto payload = context.R13;
    if (!read_float_at(payload + 0x20, ref_x) || !read_float_at(payload + 0x24, ref_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    auto delta = g_queue ? g_queue->consume_pending_delta() : std::pair<std::int64_t, std::int64_t>{0, 0};
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (delta.first != 0 || delta.second != 0) {
      g_experimental_site9_cached_x.store(delta.first, std::memory_order_release);
      g_experimental_site9_cached_y.store(delta.second, std::memory_order_release);
      g_experimental_site9_cached_qpc.store(now.QuadPart, std::memory_order_release);
    } else {
      const auto cached_qpc = g_experimental_site9_cached_qpc.load(std::memory_order_acquire);
      LARGE_INTEGER frequency{};
      QueryPerformanceFrequency(&frequency);
      const auto max_age_qpc =
          (frequency.QuadPart * static_cast<LONGLONG>(g_experimental_mouse_fix_max_age_ms.load(std::memory_order_acquire))) /
          1000;
      const auto age = now.QuadPart >= cached_qpc ? now.QuadPart - cached_qpc : cached_qpc - now.QuadPart;
      if (cached_qpc == 0 || age > max_age_qpc) {
        g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      delta.first = g_experimental_site9_cached_x.load(std::memory_order_acquire);
      delta.second = g_experimental_site9_cached_y.load(std::memory_order_acquire);
    }

    const auto replacement_x = static_cast<float>(delta.first) * gain_x;
    const auto replacement_y = static_cast<float>(delta.second) * gain_y;
    const auto replacement_current_x = ref_x + replacement_x;
    const auto replacement_current_y = ref_y + replacement_y;
    if (!std::isfinite(replacement_x) || !std::isfinite(replacement_y) ||
        !std::isfinite(replacement_current_x) || !std::isfinite(replacement_current_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (!write_float_at(payload + 0x18, replacement_current_x) ||
        !write_float_at(payload + 0x1C, replacement_current_y) ||
        !write_float_at(payload + 0x28, replacement_x) ||
        !write_float_at(payload + 0x2C, replacement_y)) {
      g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (breakpoint.site != 7) {
    return;
  }

  float replacement_x = low_float_from_xmm(context.Xmm1) * gain_x;
  float replacement_y = low_float_from_xmm(context.Xmm2) * gain_y;

  if (mode == 2) {
    const auto sample_qpc = g_candidate_b_cursor_qpc.load(std::memory_order_acquire);
    if (sample_qpc == 0) {
      g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const auto max_age_qpc =
        (frequency.QuadPart * static_cast<LONGLONG>(g_experimental_mouse_fix_max_age_ms.load(std::memory_order_acquire))) / 1000;
    const auto age = now.QuadPart >= sample_qpc ? now.QuadPart - sample_qpc : sample_qpc - now.QuadPart;
    if (age > max_age_qpc) {
      g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const auto center_x = g_experimental_mouse_fix_center_x.load(std::memory_order_acquire);
    const auto center_y = g_experimental_mouse_fix_center_y.load(std::memory_order_acquire);
    const auto cursor_x = g_candidate_b_cursor_x.load(std::memory_order_acquire);
    const auto cursor_y = g_candidate_b_cursor_y.load(std::memory_order_acquire);
    replacement_x = static_cast<float>(cursor_x - center_x) * gain_x;
    replacement_y = static_cast<float>(cursor_y - center_y) * gain_y;
  } else if (mode != 1) {
    g_experimental_patch_passthrough.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (!std::isfinite(replacement_x) || !std::isfinite(replacement_y)) {
    g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  set_low_float(context.Xmm1, replacement_x);
  set_low_float(context.Xmm2, replacement_y);
  g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
}

LONG CALLBACK internal_trace_veh(EXCEPTION_POINTERS* exception_info) {
  if (exception_info == nullptr || exception_info->ExceptionRecord == nullptr || exception_info->ContextRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  auto* context = exception_info->ContextRecord;
  const DWORD code = exception_info->ExceptionRecord->ExceptionCode;
  auto* address = reinterpret_cast<std::byte*>(exception_info->ExceptionRecord->ExceptionAddress);
  auto* breakpoint = breakpoint_for_address(address);

  if (code == EXCEPTION_BREAKPOINT && breakpoint != nullptr && breakpoint->address != nullptr) {
    if (!breakpoint->installed.load(std::memory_order_acquire)) {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    record_internal_trace(*context, *breakpoint);
    apply_experimental_mouse_fix(*context, *breakpoint);
    write_code_byte(breakpoint->address, breakpoint->original, nullptr);
    breakpoint->installed.store(false, std::memory_order_release);
    t_internal_trace_single_step = breakpoint->address;
    context->Rip = reinterpret_cast<DWORD64>(breakpoint->address);
    context->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (code == EXCEPTION_SINGLE_STEP && t_internal_trace_single_step != nullptr) {
    auto* single_step_address = t_internal_trace_single_step;
    auto* stepped_breakpoint = breakpoint_for_address(single_step_address);
    t_internal_trace_single_step = nullptr;
    if (stepped_breakpoint != nullptr && !g_stop_internal_trace.load(std::memory_order_acquire)) {
      write_code_byte(single_step_address, std::byte{0xCC}, nullptr);
      stepped_breakpoint->installed.store(true, std::memory_order_release);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

void flush_internal_trace() {
  auto read = g_internal_trace_read.load(std::memory_order_acquire);
  const auto write = g_internal_trace_write.load(std::memory_order_acquire);
  while (read < write) {
    const auto sequence = read + 1;
    auto& slot = g_internal_trace_ring[(sequence - 1) % kInternalTraceRingCapacity];
    if (slot.ready.load(std::memory_order_acquire) != sequence) {
      break;
    }

    const auto& event = slot.event;
    std::ostringstream row;
    row << event.sequence << ',' << event.qpc << ',' << event.thread_id << ",0x" << std::hex << event.rva << std::dec
        << ',' << event.site << ',' << event.segment_id << ',' << segment_label(event.segment_id) << ',' << event.delta_x << ','
        << event.delta_y << ',' << event.ref_x << ',' << event.ref_y << ','
        << event.current_x << ',' << event.current_y << ',' << event.pre_store_x << ',' << event.pre_store_y << ','
        << event.memory_ok << ',' << event.arg_memory_ok << ',' << event.stack_arg_memory_ok;
    append_float_array(row, event.rdx_f);
    append_float_array(row, event.r8_f);
    append_float_array(row, event.r9_f);
    append_float_array(row, event.stack20_f);
    row << ",0x" << std::hex << event.rax << ",0x" << event.rbx << ",0x" << event.rcx << ",0x" << event.rdx
        << ",0x" << event.r8 << ",0x" << event.r9 << ",0x" << event.r10 << ",0x" << event.r11 << ",0x"
        << event.r12 << ",0x" << event.r13 << ",0x" << event.r14 << ",0x" << event.r15 << ",0x"
        << event.call_target << ",0x" << event.stack_arg20 << ",0x" << event.stack_arg28 << ",0x" << event.rsp
        << ",0x" << event.rbp << std::dec;
    g_internal_a_csv.write_row(row.str());
    slot.ready.store(0, std::memory_order_release);
    g_internal_trace_read.store(sequence, std::memory_order_release);
    read = sequence;
  }
}

void uninstall_internal_trace() {
  g_stop_internal_trace.store(true, std::memory_order_release);
  for (auto* breakpoint : {&g_candidate_a_breakpoint,
                           &g_candidate_a_downstream_breakpoint,
                           &g_candidate_a_dispatch_breakpoint,
                           &g_candidate_a_notify_breakpoint,
                           &g_candidate_a_setter_breakpoint,
                           &g_candidate_a_handler_breakpoint,
                           &g_candidate_b_breakpoint,
                           &g_candidate_b_downstream_breakpoint,
                           &g_candidate_b_deep_breakpoint,
                           &g_candidate_b_branch_check_breakpoint,
                           &g_candidate_b_global_check_breakpoint,
                           &g_candidate_b_camera_pair_breakpoint,
                           &g_candidate_b_camera_pair_write_breakpoint}) {
    auto* address = breakpoint->address;
    if (address != nullptr && breakpoint->installed.load(std::memory_order_acquire)) {
      write_code_byte(address, breakpoint->original, nullptr);
      breakpoint->installed.store(false, std::memory_order_release);
    }
  }
  if (g_internal_veh_handle != nullptr) {
    RemoveVectoredExceptionHandler(g_internal_veh_handle);
    g_internal_veh_handle = nullptr;
  }
}

bool install_candidate_a_breakpoint() {
  const auto base = g_game_image_base.load(std::memory_order_acquire);
  const auto end = g_game_image_end.load(std::memory_order_acquire);
  const bool observe_site12 = g_observe_internal_site12.load(std::memory_order_acquire);
  const bool observe_site13 = g_observe_internal_site13.load(std::memory_order_acquire);
  auto guard_end_rva =
      kCandidateBGlobalCheckCallRva + static_cast<std::uint32_t>(kCandidateBGlobalCheckCallBytes.size());
  if (observe_site12) {
    guard_end_rva = kCandidateBCameraPairCallRva + static_cast<std::uint32_t>(kCandidateBCameraPairCallBytes.size());
  }
  if (observe_site13) {
    guard_end_rva = std::max<std::uint32_t>(
        guard_end_rva, kCandidateBCameraPairWriteRva + static_cast<std::uint32_t>(kCandidateBCameraPairWriteBytes.size()));
  }
  if (base == 0 || end <= base || base + guard_end_rva >= end) {
    g_logger.warn("Candidate A trace guard failed: game image range is unavailable");
    return false;
  }

  auto* sub_x = reinterpret_cast<std::byte*>(base + 0x01DCA6AF);
  auto* sub_y = reinterpret_cast<std::byte*>(base + 0x01DCA6B5);
  auto* store = reinterpret_cast<std::byte*>(base + kCandidateARva);
  auto* downstream_call = reinterpret_cast<std::byte*>(base + kCandidateADownstreamCallRva);
  auto* dispatch_call = reinterpret_cast<std::byte*>(base + kCandidateADispatchCallRva);
  auto* notify_call = reinterpret_cast<std::byte*>(base + kCandidateANotifyCallRva);
  auto* setter_call = reinterpret_cast<std::byte*>(base + kCandidateASetterCallRva);
  auto* handler_call = reinterpret_cast<std::byte*>(base + kCandidateAHandlerCallRva);
  auto* b_sub_x = reinterpret_cast<std::byte*>(base + 0x01DDE587);
  auto* b_sub_y = reinterpret_cast<std::byte*>(base + 0x01DDE58D);
  auto* b_store = reinterpret_cast<std::byte*>(base + kCandidateBRva);
  auto* b_downstream_call = reinterpret_cast<std::byte*>(base + kCandidateBDownstreamCallRva);
  auto* b_deep_call = reinterpret_cast<std::byte*>(base + kCandidateBDeepCallRva);
  auto* b_branch_check_call = reinterpret_cast<std::byte*>(base + kCandidateBBranchCheckCallRva);
  auto* b_global_check_call = reinterpret_cast<std::byte*>(base + kCandidateBGlobalCheckCallRva);
  auto* b_camera_pair_call = observe_site12 ? reinterpret_cast<std::byte*>(base + kCandidateBCameraPairCallRva) : nullptr;
  auto* b_camera_pair_write = observe_site13 ? reinterpret_cast<std::byte*>(base + kCandidateBCameraPairWriteRva) : nullptr;
  if (!memory_matches(sub_x, kCandidateASubXBytes.data(), kCandidateASubXBytes.size()) ||
      !memory_matches(sub_y, kCandidateASubYBytes.data(), kCandidateASubYBytes.size()) ||
      !memory_matches(store, kCandidateAStoreBytes.data(), kCandidateAStoreBytes.size()) ||
      !memory_matches(downstream_call, kCandidateADownstreamCallBytes.data(), kCandidateADownstreamCallBytes.size()) ||
      !memory_matches(dispatch_call, kCandidateADispatchCallBytes.data(), kCandidateADispatchCallBytes.size()) ||
      !memory_matches(notify_call, kCandidateANotifyCallBytes.data(), kCandidateANotifyCallBytes.size()) ||
      !memory_matches(setter_call, kCandidateASetterCallBytes.data(), kCandidateASetterCallBytes.size()) ||
      !memory_matches(handler_call, kCandidateAHandlerCallBytes.data(), kCandidateAHandlerCallBytes.size()) ||
      !memory_matches(b_sub_x, kCandidateBSubXBytes.data(), kCandidateBSubXBytes.size()) ||
      !memory_matches(b_sub_y, kCandidateBSubYBytes.data(), kCandidateBSubYBytes.size()) ||
      !memory_matches(b_store, kCandidateBStoreBytes.data(), kCandidateBStoreBytes.size()) ||
      !memory_matches(b_downstream_call, kCandidateBDownstreamCallBytes.data(), kCandidateBDownstreamCallBytes.size()) ||
      !memory_matches(b_deep_call, kCandidateBDeepCallBytes.data(), kCandidateBDeepCallBytes.size()) ||
      !memory_matches(b_branch_check_call, kCandidateBBranchCheckCallBytes.data(), kCandidateBBranchCheckCallBytes.size()) ||
      !memory_matches(b_global_check_call, kCandidateBGlobalCheckCallBytes.data(), kCandidateBGlobalCheckCallBytes.size()) ||
      (observe_site12 && !memory_matches(b_camera_pair_call, kCandidateBCameraPairCallBytes.data(), kCandidateBCameraPairCallBytes.size())) ||
      (observe_site13 &&
       !memory_matches(b_camera_pair_write, kCandidateBCameraPairWriteBytes.data(), kCandidateBCameraPairWriteBytes.size()))) {
    g_logger.warn("Candidate A trace guard failed: expected instruction bytes do not match");
    return false;
  }

  g_candidate_a_breakpoint.address = store;
  g_candidate_a_breakpoint.rva = kCandidateARva;
  g_candidate_a_breakpoint.site = 1;
  g_candidate_a_breakpoint.original = *store;
  g_candidate_a_downstream_breakpoint.address = downstream_call;
  g_candidate_a_downstream_breakpoint.rva = kCandidateADownstreamCallRva;
  g_candidate_a_downstream_breakpoint.site = 2;
  g_candidate_a_downstream_breakpoint.original = *downstream_call;
  g_candidate_a_dispatch_breakpoint.address = dispatch_call;
  g_candidate_a_dispatch_breakpoint.rva = kCandidateADispatchCallRva;
  g_candidate_a_dispatch_breakpoint.site = 3;
  g_candidate_a_dispatch_breakpoint.original = *dispatch_call;
  g_candidate_a_notify_breakpoint.address = notify_call;
  g_candidate_a_notify_breakpoint.rva = kCandidateANotifyCallRva;
  g_candidate_a_notify_breakpoint.site = 4;
  g_candidate_a_notify_breakpoint.original = *notify_call;
  g_candidate_a_setter_breakpoint.address = setter_call;
  g_candidate_a_setter_breakpoint.rva = kCandidateASetterCallRva;
  g_candidate_a_setter_breakpoint.site = 5;
  g_candidate_a_setter_breakpoint.original = *setter_call;
  g_candidate_a_handler_breakpoint.address = handler_call;
  g_candidate_a_handler_breakpoint.rva = kCandidateAHandlerCallRva;
  g_candidate_a_handler_breakpoint.site = 6;
  g_candidate_a_handler_breakpoint.original = *handler_call;
  g_candidate_b_breakpoint.address = b_store;
  g_candidate_b_breakpoint.rva = kCandidateBRva;
  g_candidate_b_breakpoint.site = 7;
  g_candidate_b_breakpoint.original = *b_store;
  g_candidate_b_downstream_breakpoint.address = b_downstream_call;
  g_candidate_b_downstream_breakpoint.rva = kCandidateBDownstreamCallRva;
  g_candidate_b_downstream_breakpoint.site = 8;
  g_candidate_b_downstream_breakpoint.original = *b_downstream_call;
  g_candidate_b_deep_breakpoint.address = b_deep_call;
  g_candidate_b_deep_breakpoint.rva = kCandidateBDeepCallRva;
  g_candidate_b_deep_breakpoint.site = 9;
  g_candidate_b_deep_breakpoint.original = *b_deep_call;
  g_candidate_b_branch_check_breakpoint.address = b_branch_check_call;
  g_candidate_b_branch_check_breakpoint.rva = kCandidateBBranchCheckCallRva;
  g_candidate_b_branch_check_breakpoint.site = 10;
  g_candidate_b_branch_check_breakpoint.original = *b_branch_check_call;
  g_candidate_b_global_check_breakpoint.address = b_global_check_call;
  g_candidate_b_global_check_breakpoint.rva = kCandidateBGlobalCheckCallRva;
  g_candidate_b_global_check_breakpoint.site = 11;
  g_candidate_b_global_check_breakpoint.original = *b_global_check_call;
  if (observe_site12) {
    g_candidate_b_camera_pair_breakpoint.address = b_camera_pair_call;
    g_candidate_b_camera_pair_breakpoint.rva = kCandidateBCameraPairCallRva;
    g_candidate_b_camera_pair_breakpoint.site = 12;
    g_candidate_b_camera_pair_breakpoint.original = *b_camera_pair_call;
  } else {
    g_candidate_b_camera_pair_breakpoint.address = nullptr;
    g_candidate_b_camera_pair_breakpoint.rva = 0;
    g_candidate_b_camera_pair_breakpoint.site = 0;
    g_candidate_b_camera_pair_breakpoint.original = std::byte{0};
    g_candidate_b_camera_pair_breakpoint.installed.store(false, std::memory_order_release);
  }
  if (observe_site13) {
    g_candidate_b_camera_pair_write_breakpoint.address = b_camera_pair_write;
    g_candidate_b_camera_pair_write_breakpoint.rva = kCandidateBCameraPairWriteRva;
    g_candidate_b_camera_pair_write_breakpoint.site = 13;
    g_candidate_b_camera_pair_write_breakpoint.original = *b_camera_pair_write;
  } else {
    g_candidate_b_camera_pair_write_breakpoint.address = nullptr;
    g_candidate_b_camera_pair_write_breakpoint.rva = 0;
    g_candidate_b_camera_pair_write_breakpoint.site = 0;
    g_candidate_b_camera_pair_write_breakpoint.original = std::byte{0};
    g_candidate_b_camera_pair_write_breakpoint.installed.store(false, std::memory_order_release);
  }
  if (g_internal_veh_handle == nullptr) {
    g_internal_veh_handle = AddVectoredExceptionHandler(1, internal_trace_veh);
    if (g_internal_veh_handle == nullptr) {
      g_logger.warn("Candidate A trace guard failed: AddVectoredExceptionHandler failed");
      return false;
    }
  }
  if (!write_code_byte(store, std::byte{0xCC}, &g_candidate_a_breakpoint.original)) {
    g_logger.warn("Candidate A trace guard failed: unable to write breakpoint");
    RemoveVectoredExceptionHandler(g_internal_veh_handle);
    g_internal_veh_handle = nullptr;
    return false;
  }
  g_candidate_a_breakpoint.installed.store(true, std::memory_order_release);
  if (!write_code_byte(downstream_call, std::byte{0xCC}, &g_candidate_a_downstream_breakpoint.original)) {
    g_logger.warn("Candidate A trace guard failed: unable to write downstream breakpoint");
    write_code_byte(store, g_candidate_a_breakpoint.original, nullptr);
    g_candidate_a_breakpoint.installed.store(false, std::memory_order_release);
    RemoveVectoredExceptionHandler(g_internal_veh_handle);
    g_internal_veh_handle = nullptr;
    return false;
  }
  g_candidate_a_downstream_breakpoint.installed.store(true, std::memory_order_release);
  if (!write_code_byte(dispatch_call, std::byte{0xCC}, &g_candidate_a_dispatch_breakpoint.original)) {
    g_logger.warn("Candidate A trace guard failed: unable to write dispatch breakpoint");
    write_code_byte(downstream_call, g_candidate_a_downstream_breakpoint.original, nullptr);
    g_candidate_a_downstream_breakpoint.installed.store(false, std::memory_order_release);
    write_code_byte(store, g_candidate_a_breakpoint.original, nullptr);
    g_candidate_a_breakpoint.installed.store(false, std::memory_order_release);
    RemoveVectoredExceptionHandler(g_internal_veh_handle);
    g_internal_veh_handle = nullptr;
    return false;
  }
  g_candidate_a_dispatch_breakpoint.installed.store(true, std::memory_order_release);
  for (auto* branch_breakpoint : {&g_candidate_a_notify_breakpoint,
                                  &g_candidate_a_setter_breakpoint,
                                  &g_candidate_a_handler_breakpoint,
                                  &g_candidate_b_breakpoint,
                                  &g_candidate_b_downstream_breakpoint,
                                  &g_candidate_b_deep_breakpoint,
                                  &g_candidate_b_branch_check_breakpoint,
                                  &g_candidate_b_global_check_breakpoint}) {
    if (!write_code_byte(branch_breakpoint->address, std::byte{0xCC}, &branch_breakpoint->original)) {
      g_logger.warn("Candidate A trace guard failed: unable to write branch-map breakpoint");
      uninstall_internal_trace();
      return false;
    }
    branch_breakpoint->installed.store(true, std::memory_order_release);
  }
  if (observe_site12) {
    if (!write_code_byte(g_candidate_b_camera_pair_breakpoint.address,
                         std::byte{0xCC},
                         &g_candidate_b_camera_pair_breakpoint.original)) {
      g_logger.warn("Candidate A trace guard failed: unable to write site12 camera-pair breakpoint");
      uninstall_internal_trace();
      return false;
    }
    g_candidate_b_camera_pair_breakpoint.installed.store(true, std::memory_order_release);
  }
  if (observe_site13) {
    if (!write_code_byte(g_candidate_b_camera_pair_write_breakpoint.address,
                         std::byte{0xCC},
                         &g_candidate_b_camera_pair_write_breakpoint.original)) {
      g_logger.warn("Candidate A trace guard failed: unable to write site13 camera-pair write breakpoint");
      uninstall_internal_trace();
      return false;
    }
    g_candidate_b_camera_pair_write_breakpoint.installed.store(true, std::memory_order_release);
  }
  g_logger.info(std::string("Candidate A/B internal ObserveOnly trace installed at A store 0x01DCA6BB, A downstream 0x01DCA7F3, A branches 0x01DCB2E5/0x01DCAEE3/0x01DCB632/0x01DCB3B2, B store 0x01DDE593, B downstream 0x01DDE62D, B dispatch 0x01DD52B4, B branch checks 0x01DD5526/0x01DD553B") +
                (observe_site12 ? ", B camera pair call 0x01DE22FD" : "; site12 disabled by ObserveInternalSite12=0") +
                (observe_site13 ? ", and B camera pair write 0x01D9675A" : "; site13 disabled by ObserveInternalSite13=0"));
  return true;
}

void internal_trace_loop(BuildFingerprint fp, std::uint32_t delay_ms) {
  if (!supported_internal_trace_fingerprint(fp)) {
    g_logger.warn("Candidate A trace disabled: executable fingerprint is not supported");
    return;
  }

  const auto delay_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
  while (!g_stop_internal_trace.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < delay_until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (g_stop_internal_trace.load(std::memory_order_acquire)) {
    return;
  }
  // ASI loaders load plugins alphabetically, so v0.1 (SADE.HighFpsRawMouseFix.asi) can show up after
  // this core has started. Two mouse fixes hooking the same calls would fight each other.
  if (GetModuleHandleW(L"SADE.HighFpsRawMouseFix.asi") != nullptr) {
    g_logger.error("Old SADE.HighFpsRawMouseFix (v0.1) is also installed. Delete scripts\\SADE.HighFpsRawMouseFix.* "
                   "and restart the game. Mouse fix NOT active.");
    return;
  }

  if (g_observe_gameplay_state.load(std::memory_order_acquire) &&
      !g_experimental_mouse_fix.load(std::memory_order_acquire)) {
    const auto hooks =
        g_set_cursor_pos_hooks.install_main_module("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&observed_set_cursor_pos));
    g_logger.warn("GameplayStateProbe delayed SetCursorPos-only observer enabled; SetCursorPos hooks=" + std::to_string(hooks));
    while (!g_stop_internal_trace.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return;
  }

  const auto experimental_mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
  if (g_experimental_mouse_fix.load(std::memory_order_acquire) &&
      (experimental_mode == 4 || experimental_mode == 5 || experimental_mode == 6 || experimental_mode == 8 ||
       experimental_mode == 10 || experimental_mode == 11 || experimental_mode == 12 || experimental_mode == 14 ||
       experimental_mode == 15 || experimental_mode == 16 || experimental_mode == 17 || experimental_mode == 18 ||
       experimental_mode == 21 || experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 ||
       experimental_mode == 25 || experimental_mode == 26 || experimental_mode == 27)) {
    const auto hooks =
        g_get_cursor_pos_hooks.install_main_module("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&observed_get_cursor_pos));
    const auto set_hooks =
        (experimental_mode == 10 || experimental_mode == 11 || experimental_mode == 12 || experimental_mode == 14 ||
         experimental_mode == 15 || experimental_mode == 16 || experimental_mode == 17 || experimental_mode == 18 ||
         experimental_mode == 21 || experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 ||
         experimental_mode == 25 || experimental_mode == 26 || experimental_mode == 27)
            ? g_set_cursor_pos_hooks.install_main_module(
                  "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&observed_set_cursor_pos))
            : 0U;
    const auto async_key_hooks =
        experimental_mode == 21
            ? g_get_async_key_state_hooks.install_main_module(
                  "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&observed_get_async_key_state))
            : 0U;
    const auto key_hooks =
        experimental_mode == 21
            ? g_get_key_state_hooks.install_main_module(
                  "USER32.dll", "GetKeyState", reinterpret_cast<void*>(&observed_get_key_state))
            : 0U;
    g_delayed_cursor_filter_active.store(true, std::memory_order_release);
    g_experimental_rinput_protocol_active.store(
        experimental_mode == 10 || experimental_mode == 11 || experimental_mode == 12 || experimental_mode == 14 ||
            experimental_mode == 15 || experimental_mode == 16 || experimental_mode == 17 || experimental_mode == 18 ||
            experimental_mode == 21 || experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 ||
            experimental_mode == 25 || experimental_mode == 26 || experimental_mode == 27,
        std::memory_order_release);
    if (experimental_mode == 10 || experimental_mode == 11 || experimental_mode == 12 || experimental_mode == 14 ||
        experimental_mode == 15 || experimental_mode == 16 || experimental_mode == 17 || experimental_mode == 18 ||
        experimental_mode == 21 || experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 ||
        experimental_mode == 25 || experimental_mode == 26 || experimental_mode == 27) {
      g_logger.warn("ExperimentalMouseFixV2 mode" + std::to_string(experimental_mode) +
                    " delayed RInput-style cursor protocol enabled; GetCursorPos hooks=" +
                    std::to_string(hooks) + " SetCursorPos hooks=" + std::to_string(set_hooks) +
                    " GetAsyncKeyState hooks=" + std::to_string(async_key_hooks) +
                    " GetKeyState hooks=" + std::to_string(key_hooks));
      if (hooks > 0 && set_hooks > 0) {
        // The line users are told to look for when checking that the mod works.
        g_logger.info("RAW MOUSE FIX ACTIVE");
      } else {
        g_logger.warn("RAW MOUSE FIX NOT ACTIVE: camera hooks could not be installed");
      }
    } else {
      g_logger.warn(std::string("ExperimentalMouseFixV2 mode") + std::to_string(experimental_mode) +
                    " delayed live cursor filter enabled for parent 0x01DDC394; GetCursorPos hooks=" + std::to_string(hooks));
    }
    if (experimental_mode != 8) {
      return;
    }
  }

  if (!install_candidate_a_breakpoint()) {
    return;
  }

  const auto max_rows = g_internal_trace_max_rows.load(std::memory_order_acquire);
  bool row_limit_reported = false;
  while (!g_stop_internal_trace.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    flush_internal_trace();
    if (max_rows != 0 && g_internal_trace_hits.load(std::memory_order_acquire) >= max_rows) {
      if (!row_limit_reported) {
        g_logger.info(g_experimental_mouse_fix.load(std::memory_order_acquire)
                          ? "Candidate A/B internal trace reached row limit; keeping experimental patch breakpoint active"
                          : "Candidate A internal ObserveOnly trace reached row limit; uninstalling breakpoint");
        row_limit_reported = true;
      }
      if (!g_experimental_mouse_fix.load(std::memory_order_acquire)) {
        break;
      }
    }
  }

  uninstall_internal_trace();
  flush_internal_trace();
}

std::uintptr_t caller_rva(void* return_address) {
  const auto address = reinterpret_cast<std::uintptr_t>(return_address);
  const auto base = g_game_image_base.load(std::memory_order_acquire);
  const auto end = g_game_image_end.load(std::memory_order_acquire);
  if (base == 0 || address < base || address >= end) {
    return 0;
  }
  return address - base;
}

std::uintptr_t parent_game_caller_rva(std::uintptr_t direct_rva) {
  void* frames[16]{};
  const USHORT captured = RtlCaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
  bool saw_direct = direct_rva == 0;
  for (USHORT i = 0; i < captured; ++i) {
    const auto rva = caller_rva(frames[i]);
    if (rva == 0) {
      continue;
    }
    if (!saw_direct) {
      saw_direct = rva == direct_rva;
      continue;
    }
    if (rva != direct_rva) {
      return rva;
    }
  }
  return 0;
}

UINT WINAPI observed_get_raw_input_data(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
  if (g_observer) {
    return g_observer->observe_data(input, command, data, size, header_size, caller_rva(_ReturnAddress()));
  }

  auto* user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    auto* original = reinterpret_cast<RawInputObserver::GetRawInputDataFn>(GetProcAddress(user32, "GetRawInputData"));
    if (original != nullptr) {
      return original(input, command, data, size, header_size);
    }
  }

  SetLastError(ERROR_PROC_NOT_FOUND);
  return static_cast<UINT>(-1);
}

UINT WINAPI observed_get_raw_input_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
  if (g_observer) {
    return g_observer->observe_buffer(data, size, header_size);
  }

  auto* user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    auto* original = reinterpret_cast<RawInputObserver::GetRawInputBufferFn>(GetProcAddress(user32, "GetRawInputBuffer"));
    if (original != nullptr) {
      return original(data, size, header_size);
    }
  }

  SetLastError(ERROR_PROC_NOT_FOUND);
  return static_cast<UINT>(-1);
}

BOOL WINAPI observed_register_raw_input_devices(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
  if (g_observer) {
    const BOOL result = g_observer->observe_register_devices(devices, count, size);
    if (result) {
      on_game_raw_input_registration(devices, count);
    }
    return result;
  }

  auto* user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    auto* original =
        reinterpret_cast<RawInputObserver::RegisterRawInputDevicesFn>(GetProcAddress(user32, "RegisterRawInputDevices"));
    if (original != nullptr) {
      return original(devices, count, size);
    }
  }

  SetLastError(ERROR_PROC_NOT_FOUND);
  return FALSE;
}

LRESULT CALLBACK raw_input_sink_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_INPUT && g_observer) {
    UINT size = 0;
    g_observer->observe_data(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER), 0);
    if (size >= sizeof(RAWINPUTHEADER) && size <= 4096) {
      std::vector<std::byte> buffer(size);
      g_observer->observe_data(
          reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER), 0);
    }
    return 0;
  }
  if (message == WM_CLOSE) {
    DestroyWindow(hwnd);
    return 0;
  }
  if (message == WM_NCDESTROY) {
    g_raw_input_sink_hwnd.store(nullptr, std::memory_order_release);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void raw_input_sink_loop() {
  auto* instance = g_self_module.load(std::memory_order_acquire);
  constexpr wchar_t kClassName[] = L"GTASADE.PCFix.RawInputSink";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = raw_input_sink_wnd_proc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(0, kClassName, kClassName, 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    g_logger.warn("ExperimentalMouseFixV2 mode9 RawInput sink failed: CreateWindowExW failed");
    return;
  }
  g_raw_input_sink_hwnd.store(hwnd, std::memory_order_release);

  RAWINPUTDEVICE device{};
  device.usUsagePage = 0x01;
  device.usUsage = 0x02;
  const auto experimental_mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
  device.dwFlags =
      (experimental_mode == 10 || experimental_mode == 11 || experimental_mode == 12 || experimental_mode == 14 ||
       experimental_mode == 15 || experimental_mode == 16 || experimental_mode == 17 || experimental_mode == 18 ||
       experimental_mode == 21 || experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 ||
       experimental_mode == 25 || experimental_mode == 26 || experimental_mode == 27)
          ? 0
          : RIDEV_INPUTSINK;
  device.hwndTarget = hwnd;
  if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
    g_logger.warn(std::string("ExperimentalMouseFixV2 mode") + std::to_string(experimental_mode) +
                  " RawInput sink failed: RegisterRawInputDevices failed");
  } else {
    g_logger.warn(std::string("ExperimentalMouseFixV2 mode") + std::to_string(experimental_mode) +
                  " RawInput sink registered mouse input flags=" + std::to_string(device.dwFlags));
  }

  MSG message{};
  while (!g_stop_raw_input_sink.load(std::memory_order_acquire) && GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (IsWindow(hwnd)) {
    DestroyWindow(hwnd);
  }
  UnregisterClassW(kClassName, instance);
}

FARPROC WINAPI observed_get_proc_address(HMODULE module, LPCSTR proc_name) {
  const auto original = g_original_get_proc_address.load(std::memory_order_acquire);
  if (original == nullptr) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return nullptr;
  }

  FARPROC result = original(module, proc_name);
  if (proc_name == nullptr || HIWORD(proc_name) == 0) {
    return result;
  }

  bool replaced = false;
  if (std::strcmp(proc_name, "GetRawInputData") == 0) {
    result = reinterpret_cast<FARPROC>(&observed_get_raw_input_data);
    replaced = true;
  } else if (std::strcmp(proc_name, "GetRawInputBuffer") == 0) {
    result = reinterpret_cast<FARPROC>(&observed_get_raw_input_buffer);
    replaced = true;
  } else if (std::strcmp(proc_name, "RegisterRawInputDevices") == 0) {
    result = reinterpret_cast<FARPROC>(&observed_register_raw_input_devices);
    replaced = true;
  } else if (g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 21 &&
             std::strcmp(proc_name, "GetAsyncKeyState") == 0) {
    result = reinterpret_cast<FARPROC>(&observed_get_async_key_state);
    replaced = true;
  } else if (g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 21 &&
             std::strcmp(proc_name, "GetKeyState") == 0) {
    result = reinterpret_cast<FARPROC>(&observed_get_key_state);
    replaced = true;
  }

  if (g_observer && (replaced || std::strstr(proc_name, "RawInput") != nullptr)) {
    g_observer->observe_get_proc_address_request(proc_name, replaced);
  }
  return result;
}

BOOL WINAPI observed_get_cursor_pos(LPPOINT point) {
  const auto original = g_original_get_cursor_pos.load(std::memory_order_acquire);
  if (original == nullptr) {
    return FALSE;
  }
  const BOOL result = original(point);
  if (point != nullptr) {
    const auto direct = caller_rva(_ReturnAddress());
    const auto parent = parent_game_caller_rva(direct);
    const auto button_mask = cursor_button_mask();
    cursor_protocol_stats(parent, button_mask).get.fetch_add(1, std::memory_order_relaxed);
    const auto experimental_mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
    if (result && g_experimental_rinput_protocol_active.load(std::memory_order_acquire) &&
        g_experimental_mouse_fix.load(std::memory_order_acquire) &&
        (experimental_mode == 10 || experimental_mode == 11 || experimental_mode == 12 || experimental_mode == 14 ||
         experimental_mode == 15 || experimental_mode == 16 || experimental_mode == 17 || experimental_mode == 18 ||
         experimental_mode == 21 || experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 ||
         experimental_mode == 25 || experimental_mode == 26 || experimental_mode == 27) &&
        direct != 0) {
      const bool inactive_mode11 = experimental_mode == 11 && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0;
      const bool inactive_mode12 = experimental_mode == 12 && !experimental_recent_center_warp_active();
      const bool inactive_mode14 =
          experimental_mode == 14 && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 &&
          ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 || !experimental_recent_center_warp_active());
      const bool inactive_mode15 = experimental_mode == 15 && parent != kLiveActionGetCursorParentRva;
      const bool inactive_mode16 =
          experimental_mode == 16 && (parent != kLiveActionGetCursorParentRva || !experimental_recent_center_warp_armed());
      const bool inactive_mode17 = experimental_mode == 17 && parent != kLiveActionGetCursorParentRva;
      const bool inactive_mode18 =
          experimental_mode == 18 && (parent != kLiveActionGetCursorParentRva || !experimental_recent_center_warp_armed());
      const bool inactive_mode21 =
          experimental_mode == 21 && (parent != kLiveActionGetCursorParentRva || !experimental_recent_center_warp_armed());
      const bool inactive_mode22 =
          (experimental_mode == 22 || experimental_mode == 23 || experimental_mode == 24 || experimental_mode == 25 ||
           experimental_mode == 26 || experimental_mode == 27) &&
          !gameplay_state_recent_active();
      const bool inactive_mode27 = experimental_mode == 27 && parent != kLiveActionGetCursorParentRva;
      // Disabled by the user: drain companion deltas and let the game see its real cursor.
      const bool user_disabled = !g_user_enabled.load(std::memory_order_acquire);
      if (user_disabled || inactive_mode11 || inactive_mode12 || inactive_mode14 || inactive_mode15 || inactive_mode16 || inactive_mode17 ||
          inactive_mode18 || inactive_mode21 || inactive_mode22 || inactive_mode27) {
        if ((experimental_mode == 26 || experimental_mode == 27) && (!inactive_mode27 || user_disabled)) {
          const auto drained = consume_protocol_delta();
          if (!inactive_mode27) {
            trace_cursor("get", user_disabled ? "disabled" : "passthrough", parent, button_mask, drained.first,
                         drained.second, point->x, point->y);
          }
        } else if (g_queue && !inactive_mode15 && !inactive_mode16 && !inactive_mode17 && !inactive_mode18 && !inactive_mode21 &&
            !inactive_mode22) {
          (void)g_queue->consume_pending_delta();
        }
        g_experimental_patch_passthrough.fetch_add(1, std::memory_order_relaxed);
        cursor_protocol_stats(parent, button_mask).passthrough.fetch_add(1, std::memory_order_relaxed);
      } else {
        const auto gain_x = float_from_bits(g_experimental_mouse_fix_gain_x_bits.load(std::memory_order_acquire));
        const auto gain_y = float_from_bits(g_experimental_mouse_fix_gain_y_bits.load(std::memory_order_acquire));
        const auto delta = consume_protocol_delta();
        const char* action = "stale";
        if (delta.first != 0 || delta.second != 0) {
          const auto current_x = g_experimental_virtual_cursor_x.load(std::memory_order_acquire);
          const auto current_y = g_experimental_virtual_cursor_y.load(std::memory_order_acquire);
          // Keep the fractional part for the next call instead of rounding it away.
          const auto scaled_x = (static_cast<double>(delta.first) * gain_x) + g_subpixel_x.load(std::memory_order_relaxed);
          const auto scaled_y = (static_cast<double>(delta.second) * gain_y) + g_subpixel_y.load(std::memory_order_relaxed);
          if (std::isfinite(scaled_x) && std::isfinite(scaled_y)) {
            const auto whole_x = std::trunc(scaled_x);
            const auto whole_y = std::trunc(scaled_y);
            g_subpixel_x.store(scaled_x - whole_x, std::memory_order_relaxed);
            g_subpixel_y.store(scaled_y - whole_y, std::memory_order_relaxed);
            g_experimental_virtual_cursor_x.store(current_x + static_cast<std::int32_t>(whole_x), std::memory_order_release);
            g_experimental_virtual_cursor_y.store(current_y + static_cast<std::int32_t>(whole_y), std::memory_order_release);
            g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
            cursor_protocol_stats(parent, button_mask).applied.fetch_add(1, std::memory_order_relaxed);
            action = "applied";
          } else {
            g_subpixel_x.store(0.0, std::memory_order_relaxed);
            g_subpixel_y.store(0.0, std::memory_order_relaxed);
            g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
            cursor_protocol_stats(parent, button_mask).invalid.fetch_add(1, std::memory_order_relaxed);
            action = "invalid";
          }
        } else {
          g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
          cursor_protocol_stats(parent, button_mask).stale.fetch_add(1, std::memory_order_relaxed);
        }
        point->x = static_cast<LONG>(g_experimental_virtual_cursor_x.load(std::memory_order_acquire));
        point->y = static_cast<LONG>(g_experimental_virtual_cursor_y.load(std::memory_order_acquire));
        trace_cursor("get", action, parent, button_mask, delta.first, delta.second, point->x, point->y);
      }
    }
    if (result && g_delayed_cursor_filter_active.load(std::memory_order_acquire) &&
        g_experimental_mouse_fix.load(std::memory_order_acquire) &&
        (experimental_mode == 4 || experimental_mode == 5 || experimental_mode == 6) &&
        parent == kLiveActionGetCursorParentRva) {
      const auto gain_x = float_from_bits(g_experimental_mouse_fix_gain_x_bits.load(std::memory_order_acquire));
      const auto gain_y = float_from_bits(g_experimental_mouse_fix_gain_y_bits.load(std::memory_order_acquire));
      const auto center_x = g_experimental_mouse_fix_center_x.load(std::memory_order_acquire);
      const auto center_y = g_experimental_mouse_fix_center_y.load(std::memory_order_acquire);
      float replacement_x = 0.0F;
      float replacement_y = 0.0F;
      if (experimental_mode == 6) {
        const auto delta = g_queue ? g_queue->consume_pending_delta() : std::pair<std::int64_t, std::int64_t>{0, 0};
        if (delta.first != 0 || delta.second != 0) {
          g_experimental_virtual_cursor_idle_calls.store(0, std::memory_order_release);
          const auto current_x = g_experimental_virtual_cursor_x.load(std::memory_order_acquire);
          const auto current_y = g_experimental_virtual_cursor_y.load(std::memory_order_acquire);
          const auto min_x = center_x - kExperimentalVirtualCursorMaxOffsetX;
          const auto max_x = center_x + kExperimentalVirtualCursorMaxOffsetX;
          const auto min_y = center_y - kExperimentalVirtualCursorMaxOffsetY;
          const auto max_y = center_y + kExperimentalVirtualCursorMaxOffsetY;
          const auto next_x = static_cast<std::int32_t>(std::clamp(
              static_cast<long long>(current_x) + static_cast<long long>(std::lround(static_cast<double>(delta.first) * gain_x)),
              static_cast<long long>(min_x),
              static_cast<long long>(max_x)));
          const auto next_y = static_cast<std::int32_t>(std::clamp(
              static_cast<long long>(current_y) + static_cast<long long>(std::lround(static_cast<double>(delta.second) * gain_y)),
              static_cast<long long>(min_y),
              static_cast<long long>(max_y)));
          g_experimental_virtual_cursor_x.store(next_x, std::memory_order_release);
          g_experimental_virtual_cursor_y.store(next_y, std::memory_order_release);
        } else {
          const auto idle_calls = g_experimental_virtual_cursor_idle_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
          g_experimental_patch_stale.fetch_add(1, std::memory_order_relaxed);
          if (idle_calls > kExperimentalVirtualCursorIdleResetCalls) {
            g_experimental_virtual_cursor_x.store(center_x, std::memory_order_release);
            g_experimental_virtual_cursor_y.store(center_y, std::memory_order_release);
          }
        }
        replacement_x = static_cast<float>(g_experimental_virtual_cursor_x.load(std::memory_order_acquire));
        replacement_y = static_cast<float>(g_experimental_virtual_cursor_y.load(std::memory_order_acquire));
      } else if (experimental_mode == 5) {
        const auto delta = g_queue ? g_queue->consume_pending_delta() : std::pair<std::int64_t, std::int64_t>{0, 0};
        replacement_x = static_cast<float>(center_x) + (static_cast<float>(delta.first) * gain_x);
        replacement_y = static_cast<float>(center_y) + (static_cast<float>(delta.second) * gain_y);
      } else {
        replacement_x = static_cast<float>(center_x) + (static_cast<float>(point->x - center_x) * gain_x);
        replacement_y = static_cast<float>(center_y) + (static_cast<float>(point->y - center_y) * gain_y);
      }
      if (std::isfinite(replacement_x) && std::isfinite(replacement_y)) {
        point->x = static_cast<LONG>(std::lround(replacement_x));
        point->y = static_cast<LONG>(std::lround(replacement_y));
        g_experimental_patch_applied.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_experimental_patch_invalid.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (result && g_experimental_mouse_fix.load(std::memory_order_acquire) &&
        g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 2 && parent == kCandidateBGetCursorParentRva) {
      LARGE_INTEGER qpc{};
      QueryPerformanceCounter(&qpc);
      g_candidate_b_cursor_x.store(point->x, std::memory_order_release);
      g_candidate_b_cursor_y.store(point->y, std::memory_order_release);
      g_candidate_b_cursor_qpc.store(qpc.QuadPart, std::memory_order_release);
    }
    if (g_observer) {
      g_observer->observe_cursor_point("get_cursor_pos", result, point->x, point->y, direct, parent);
    }
  }
  return result;
}

SHORT WINAPI observed_get_async_key_state(int vkey) {
  const auto original = g_original_get_async_key_state.load(std::memory_order_acquire);
  const SHORT result = original != nullptr ? original(vkey) : 0;
  if (vkey == VK_RBUTTON && g_experimental_mouse_fix.load(std::memory_order_acquire) &&
      g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 21 &&
      g_experimental_rinput_protocol_active.load(std::memory_order_acquire) && experimental_recent_center_warp_armed()) {
    g_experimental_key_state_forced.fetch_add(1, std::memory_order_relaxed);
    return static_cast<SHORT>(result | 0x8000);
  }
  return result;
}

SHORT WINAPI observed_get_key_state(int vkey) {
  const auto original = g_original_get_key_state.load(std::memory_order_acquire);
  const SHORT result = original != nullptr ? original(vkey) : 0;
  if (vkey == VK_RBUTTON && g_experimental_mouse_fix.load(std::memory_order_acquire) &&
      g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 21 &&
      g_experimental_rinput_protocol_active.load(std::memory_order_acquire) && experimental_recent_center_warp_armed()) {
    g_experimental_key_state_forced.fetch_add(1, std::memory_order_relaxed);
    return static_cast<SHORT>(result | 0x8000);
  }
  return result;
}

BOOL WINAPI observed_set_cursor_pos(int x, int y) {
  const auto original = g_original_set_cursor_pos.load(std::memory_order_acquire);
  if (original == nullptr) {
    return FALSE;
  }
  const BOOL result = original(x, y);
  if (g_observer) {
    const auto direct = caller_rva(_ReturnAddress());
    const auto parent = parent_game_caller_rva(direct);
    cursor_protocol_stats(parent, cursor_button_mask()).set.fetch_add(1, std::memory_order_relaxed);
    if (result) {
      gameplay_state_record_set_cursor(x, y, direct, parent);
      trace_cursor("set", "set", parent, cursor_button_mask(), 0, 0, x, y);
    }
    if (result && g_experimental_rinput_protocol_active.load(std::memory_order_acquire) &&
        g_experimental_mouse_fix.load(std::memory_order_acquire) &&
        (g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 10 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 11 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 12 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 14 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 15 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 16 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 17 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 18 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 21 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 22 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 23 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 24 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 25 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 26 ||
         g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 27) &&
        direct != 0) {
      const auto experimental_mode = g_experimental_mouse_fix_mode.load(std::memory_order_acquire);
      // Modes 24-27 leave raw delta for GetCursorPos while camera capture is active.
      // Outside gameplay it is drained here so menu motion cannot accumulate.
      if (g_queue && experimental_mode != 18 &&
          ((experimental_mode != 24 && experimental_mode != 25 && experimental_mode != 26 && experimental_mode != 27) ||
           !gameplay_state_recent_active())) {
        (void)consume_protocol_delta();
      }
      g_experimental_mouse_fix_center_x.store(x, std::memory_order_release);
      g_experimental_mouse_fix_center_y.store(y, std::memory_order_release);
      g_experimental_virtual_cursor_x.store(x, std::memory_order_release);
      g_experimental_virtual_cursor_y.store(y, std::memory_order_release);
      // The game's own recenter call site, independent of the screen resolution.
      if (direct == kSetCursorCenterReturnRva) {
        experimental_record_center_warp();
      }
    }
    if (result && g_experimental_mouse_fix.load(std::memory_order_acquire) && direct == kSetCursorCenterReturnRva) {
      g_experimental_mouse_fix_center_x.store(x, std::memory_order_release);
      g_experimental_mouse_fix_center_y.store(y, std::memory_order_release);
    }
    g_observer->observe_cursor_point("set_cursor_pos", result, x, y, direct, parent);
  }
  return result;
}

BOOL WINAPI observed_clip_cursor(const RECT* rect) {
  const auto original = g_original_clip_cursor.load(std::memory_order_acquire);
  if (original == nullptr) {
    return FALSE;
  }
  const BOOL result = original(rect);
  if (g_observer) {
    const auto direct = caller_rva(_ReturnAddress());
    g_observer->observe_cursor_rect("clip_cursor", result, rect, direct, parent_game_caller_rva(direct));
  }
  return result;
}

BOOL WINAPI observed_get_clip_cursor(LPRECT rect) {
  const auto original = g_original_get_clip_cursor.load(std::memory_order_acquire);
  if (original == nullptr) {
    return FALSE;
  }
  const BOOL result = original(rect);
  if (g_observer) {
    const auto direct = caller_rva(_ReturnAddress());
    g_observer->observe_cursor_rect("get_clip_cursor", result, rect, direct, parent_game_caller_rva(direct));
  }
  return result;
}

std::string narrow_lossy(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::optional<std::wstring> local_app_data_path() {
  DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) {
    return std::nullopt;
  }
  std::wstring value(required, L'\0');
  const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
  if (written == 0 || written >= required) {
    return std::nullopt;
  }
  value.resize(written);
  return value;
}

void write_profile_value(const std::wstring& path,
                         const wchar_t* section,
                         const wchar_t* key,
                         const wchar_t* value) {
  if (!WritePrivateProfileStringW(section, key, value, path.c_str())) {
    g_logger.warn("ForceUnlimitedFps failed to write " + narrow_lossy(path));
  }
}

void apply_force_unlimited_fps_config() {
  const auto local_app_data = local_app_data_path();
  if (!local_app_data) {
    g_logger.warn("ForceUnlimitedFps requested but LOCALAPPDATA is unavailable");
    return;
  }

  const auto config_dir =
      std::filesystem::path(*local_app_data) / L"Rockstar Games" / L"GTA San Andreas Definitive Edition" / L"Saved" /
      L"Config" / L"WindowsNoEditor";
  std::error_code create_error;
  std::filesystem::create_directories(config_dir, create_error);
  if (create_error) {
    g_logger.warn("ForceUnlimitedFps failed to create config directory: " + narrow_lossy(config_dir.wstring()));
    return;
  }

  const auto game_user_settings = (config_dir / L"GameUserSettings.ini").wstring();
  write_profile_value(game_user_settings, L"/Script/GTABase.GameterSettings", L"FrameRateLimit", L"0.000000");
  write_profile_value(game_user_settings, L"/Script/GTABase.GameterSettings", L"bUseVSync", L"False");
  write_profile_value(game_user_settings, L"/Script/Engine.GameUserSettings", L"FrameRateLimit", L"0.000000");
  write_profile_value(game_user_settings, L"/Script/Engine.GameUserSettings", L"bUseVSync", L"False");

  const auto engine_ini = (config_dir / L"Engine.ini").wstring();
  write_profile_value(engine_ini, L"/Script/Engine.Engine", L"bSmoothFrameRate", L"False");
  write_profile_value(engine_ini,
                      L"/Script/Engine.Engine",
                      L"SmoothedFrameRateRange",
                      L"(LowerBound=(Type=\"ERangeBoundTypes::Inclusive\",Value=5),UpperBound=(Type=\"ERangeBoundTypes::Inclusive\",Value=0))");
  write_profile_value(engine_ini, L"SystemSettings", L"r.VSync", L"0");
  write_profile_value(engine_ini, L"SystemSettings", L"t.MaxFPS", L"0");

  g_logger.info("ForceUnlimitedFps wrote unlimited FPS overrides to " + narrow_lossy(game_user_settings));
}

std::wstring run_stamp_utc() {
  SYSTEMTIME st{};
  GetSystemTime(&st);
  wchar_t buffer[32]{};
  swprintf_s(buffer,
             L"%04u%02u%02u_%02u%02u%02u",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond);
  return buffer;
}

void write_fingerprint(const BuildFingerprint& fp) {
  std::ostringstream out;
  out << "Game executable: " << narrow_lossy(fp.path);
  g_logger.info(out.str());
  g_logger.info("FileVersion=" + fp.file_version);
  g_logger.info("Size=" + std::to_string(fp.size));
  g_logger.info("SHA256=" + fp.sha256);
  g_logger.info("TextSHA256=" + fp.text_sha256);
  g_logger.info("PETimestamp=" + std::to_string(fp.pe_timestamp));
  g_logger.info("ImageBase=0x" + [&] {
                  std::ostringstream s;
                  s << std::hex << fp.image_base;
                  return s.str();
                }());
  g_logger.info(std::string("IsX64=") + (fp.is_x64 ? "true" : "false"));
}

void timing_loop() {
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  std::uint64_t sequence = 0;
  auto last = std::chrono::steady_clock::now();

  while (!g_stop_timing.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    last = now;

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const auto stats = g_queue ? g_queue->statistics() : RawInputStatistics{};
    std::ostringstream row;
    row << ++sequence << ',' << qpc.QuadPart << ',' << frequency.QuadPart << ',' << elapsed_ms << ',' << stats.observed << ','
        << stats.buffered_observed << ',' << stats.duplicates << ',' << stats.size_queries << ',' << stats.buffer_size_queries << ','
        << stats.non_mouse << ',' << stats.errors << ',' << stats.buffer_errors << ',' << stats.registrations << ','
        << stats.overflow << ',' << stats.accumulated_x << ',' << stats.accumulated_y << ',' << g_get_raw_input_data_hooks.count()
        << ',' << g_get_raw_input_buffer_hooks.count() << ',' << g_register_raw_input_devices_hooks.count() << ','
        << g_get_proc_address_hooks.count() << ',' << stats.wm_input_messages << ',' << g_get_cursor_pos_hooks.count() << ','
        << g_set_cursor_pos_hooks.count() << ',' << g_clip_cursor_hooks.count() << ',' << g_get_clip_cursor_hooks.count();
    g_timing_csv.write_row(row.str());

    if (sequence % 4 == 0) {
      g_get_raw_input_data_hooks.install_main_module(
          "USER32.dll", "GetRawInputData", reinterpret_cast<void*>(&observed_get_raw_input_data));
      g_get_raw_input_buffer_hooks.install_main_module(
          "USER32.dll", "GetRawInputBuffer", reinterpret_cast<void*>(&observed_get_raw_input_buffer));
      g_register_raw_input_devices_hooks.install_main_module(
          "USER32.dll", "RegisterRawInputDevices", reinterpret_cast<void*>(&observed_register_raw_input_devices));
      if (g_observe_get_proc_address.load(std::memory_order_acquire)) {
        g_get_proc_address_hooks.install_main_module(
            "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&observed_get_proc_address), g_self_module.load());
      }
      if (g_observe_cursor_apis.load(std::memory_order_acquire)) {
        g_get_cursor_pos_hooks.install_main_module("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&observed_get_cursor_pos));
        g_set_cursor_pos_hooks.install_main_module("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&observed_set_cursor_pos));
        g_clip_cursor_hooks.install_main_module("USER32.dll", "ClipCursor", reinterpret_cast<void*>(&observed_clip_cursor));
        g_get_clip_cursor_hooks.install_main_module(
            "USER32.dll", "GetClipCursor", reinterpret_cast<void*>(&observed_get_clip_cursor));
      } else if (g_observe_capture_apis.load(std::memory_order_acquire)) {
        g_set_cursor_pos_hooks.install_main_module("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&observed_set_cursor_pos));
        g_clip_cursor_hooks.install_main_module("USER32.dll", "ClipCursor", reinterpret_cast<void*>(&observed_clip_cursor));
        g_get_clip_cursor_hooks.install_main_module(
            "USER32.dll", "GetClipCursor", reinterpret_cast<void*>(&observed_get_clip_cursor));
      }
      if (g_delayed_cursor_filter_active.load(std::memory_order_acquire)) {
        g_get_cursor_pos_hooks.install_main_module(
            "USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&observed_get_cursor_pos));
        if (g_experimental_rinput_protocol_active.load(std::memory_order_acquire)) {
          g_set_cursor_pos_hooks.install_main_module(
              "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&observed_set_cursor_pos));
        }
        if (g_experimental_mouse_fix_mode.load(std::memory_order_acquire) == 21) {
          g_get_async_key_state_hooks.install_main_module(
              "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&observed_get_async_key_state));
          g_get_key_state_hooks.install_main_module(
              "USER32.dll", "GetKeyState", reinterpret_cast<void*>(&observed_get_key_state));
        }
      }
    }
  }
}

}  // namespace

ObserveOnlyRuntime& runtime() {
  static ObserveOnlyRuntime instance;
  return instance;
}

bool ObserveOnlyRuntime::start(HMODULE self_module) {
  if (running_) {
    return true;
  }

  // The core can be reached through several loaders (ASI loader from scripts\ or the game root,
  // ReShade add-on). Different file copies would be different modules, so guard per process.
  const auto instance_name = L"Local\\GTASADE.PCFix.Runtime." + std::to_wstring(GetCurrentProcessId());
  g_instance_mutex = CreateMutexW(nullptr, FALSE, instance_name.c_str());
  if (g_instance_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (g_instance_mutex != nullptr) {
      CloseHandle(g_instance_mutex);
      g_instance_mutex = nullptr;
    }
    return false;
  }

  g_self_module.store(self_module, std::memory_order_release);
  const auto asi_dir = module_directory(self_module);
  const auto ini_path = join_path(asi_dir, L"GTASADE.PCFix.ini");
  const auto run_stamp = run_stamp_utc();
  auto config = load_config(ini_path, asi_dir);

  // Regular users get one small log that is overwritten each run. Developer mode writes every
  // trace into <LogDirectory>\<run stamp>\ and keeps all runs.
  std::wstring run_directory;
  bool developer_mode = config.developer_mode;
  if (developer_mode) {
    const std::wstring base = config.developer_log_directory.empty()
                                  ? join_path(asi_dir, L"GTASADE.PCFix_logs")
                                  : config.developer_log_directory;
    run_directory = join_path(base, run_stamp);
    std::error_code ec;
    std::filesystem::create_directories(run_directory, ec);
    if (ec) {
      developer_mode = false;
      run_directory.clear();
    }
  }
  if (developer_mode) {
    config.observe_raw_input = true;
    config.observe_timing = true;
  } else {
    // Diagnostics format and buffer rows on game threads; keep them out of normal play.
    config.observe_raw_input = false;
    config.observe_timing = false;
    config.observe_markers = false;
    config.observe_cursor_apis = false;
    config.observe_get_proc_address = false;
    config.observe_internal_candidate_a = false;
  }
  config.log_directory = developer_mode ? run_directory : asi_dir;
  {
    std::lock_guard lock(g_settings_mutex);
    g_ini_path = ini_path;
    g_developer_log_directory = config.developer_log_directory;
    g_developer_run_directory = run_directory;
  }
  g_developer_mode_configured.store(config.developer_mode, std::memory_order_release);
  g_developer_mode_active.store(developer_mode, std::memory_order_release);

  const auto log_path = join_path(developer_mode ? run_directory : asi_dir, L"GTASADE.PCFix.log");
  g_logger.open(log_path);
  reset_cursor_protocol_stats();
  g_logger.info("GTASADE.PCFix " SADE_CORE_VERSION " starting");
  g_logger.info("RunStampUtc=" + narrow_lossy(run_stamp));
  g_logger.info(std::string("DeveloperMode=") + (developer_mode ? "true RunDirectory=" + narrow_lossy(run_directory) : "false"));
  if (developer_mode) {
    std::error_code ec;
    std::filesystem::copy_file(ini_path, join_path(run_directory, L"GTASADE.PCFix.ini"),
                               std::filesystem::copy_options::overwrite_existing, ec);
  }
  g_user_enabled.store(config.enabled, std::memory_order_release);
  if (config.force_unlimited_fps) {
    apply_force_unlimited_fps_config();
  }
  g_fps_lock.set_target(config.max_fps);
  const bool experimental_mouse_fix_enabled = config.experimental_mouse_fix_v2;
  const std::uint32_t experimental_mouse_fix_mode =
      experimental_mouse_fix_enabled && config.experimental_mouse_fix_mode >= 3U &&
              config.experimental_mouse_fix_mode <= 27U
          ? config.experimental_mouse_fix_mode
          : (experimental_mouse_fix_enabled ? 1U : config.experimental_mouse_fix_mode);
  const auto& log_directory = config.log_directory;
  const auto raw_csv_path = join_path(log_directory, L"rawinput.csv");
  const auto timing_csv_path = join_path(log_directory, L"timing.csv");
  const auto internal_a_csv_path = join_path(log_directory, L"internal_a.csv");
  const auto marker_csv_path = join_path(log_directory, L"markers.csv");
  if (developer_mode) {
    g_raw_csv.open(raw_csv_path,
                   "sequence,qpc,source,reserved,hrawinput,device,result,command,flags,x,y,button_flags,button_data,deduplicated");
    g_timing_csv.open(timing_csv_path,
                      "sequence,qpc,qpc_frequency,elapsed_ms,raw_observed,raw_buffered_observed,raw_duplicates,raw_size_queries,raw_buffer_size_queries,raw_non_mouse,raw_errors,raw_buffer_errors,raw_registrations,raw_overflow,accumulated_x,accumulated_y,hook_get_raw_input_data,hook_get_raw_input_buffer,hook_register_raw_input_devices,hook_get_proc_address,wm_input_messages,hook_get_cursor_pos,hook_set_cursor_pos,hook_clip_cursor,hook_get_clip_cursor");
    g_cursor_csv.open(join_path(log_directory, L"cursor.csv"),
                      "qpc,frame,kind,action,parent_rva,buttons,gameplay,raw_dx,raw_dy,virtual_x,virtual_y,out_x,out_y,"
                      "subpixel_x,subpixel_y,companion_last_qpc");
    g_frames_csv.open(join_path(log_directory, L"frames.csv"), "qpc,frame");
    g_stop_trace_flush.store(false, std::memory_order_release);
    g_trace_flush_thread = std::thread([] {
      while (!g_stop_trace_flush.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        g_raw_csv.flush();
        g_timing_csv.flush();
        g_cursor_csv.flush();
        g_frames_csv.flush();
        g_ingame_csv.flush();
        g_internal_a_csv.flush();
        g_marker_csv.flush();
      }
    });
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_logger.info("QpcFrequency=" + std::to_string(frequency.QuadPart));
  }
  if (config.observe_markers) {
    g_marker_csv.open(marker_csv_path, "qpc,timestamp_utc,thread_id,segment_id,label");
    g_current_segment_id.store(0, std::memory_order_release);
    g_stop_markers.store(false, std::memory_order_release);
    g_marker_thread = std::thread(marker_loop);
    g_logger.info("Started marker observer thread: F6=menu F7=load_game Ctrl+F8=in_car_start F8=in_car_mouse Ctrl+F9=on_foot_start F9=on_foot_mouse Shift+F9=stop_segment");
  }

  const auto exe_path = process_exe_path();
  std::optional<BuildFingerprint> game_fingerprint;
  if (const auto fp = collect_build_fingerprint(exe_path)) {
    game_fingerprint = *fp;
    write_fingerprint(*fp);
    g_supported_build.store(supported_internal_trace_fingerprint(*fp), std::memory_order_release);
    {
      std::lock_guard lock(g_settings_mutex);
      g_game_version = fp->file_version;
    }
    if (!g_supported_build.load(std::memory_order_acquire)) {
      g_logger.warn("Unsupported SanAndreas.exe build: the mouse fix stays inactive until the mod is updated for it");
    } else if (HMODULE exe_module = GetModuleHandleW(nullptr); exe_module != nullptr && fp->size_of_image > 0) {
      const auto base = reinterpret_cast<std::uintptr_t>(exe_module);
      g_fps_lock.start(base, base + fp->size_of_image, [](const std::string& message) { g_logger.info(message); });
      g_logger.info("FpsLock started, target " + std::to_string(g_fps_lock.target()) + " (0 = game decides)");
    }
    if (HMODULE exe_module = GetModuleHandleW(nullptr); exe_module != nullptr && fp->size_of_image > 0) {
      const auto base = reinterpret_cast<std::uintptr_t>(exe_module);
      g_game_image_base.store(base, std::memory_order_release);
      g_game_image_end.store(base + fp->size_of_image, std::memory_order_release);
    }
  } else {
    g_logger.warn("Unable to collect game executable fingerprint");
  }

  g_queue = std::make_unique<RawInputQueue>(config.ring_capacity);
  g_observer = std::make_unique<RawInputObserver>(*g_queue, g_raw_csv);
  g_observe_get_proc_address.store(config.observe_get_proc_address, std::memory_order_release);
  g_observe_gameplay_state.store(config.observe_gameplay_state ||
                                     (experimental_mouse_fix_enabled &&
                                      (experimental_mouse_fix_mode == 22U || experimental_mouse_fix_mode == 23U ||
                                       experimental_mouse_fix_mode == 24U || experimental_mouse_fix_mode == 25U ||
                                       experimental_mouse_fix_mode == 26U || experimental_mouse_fix_mode == 27U)),
                                 std::memory_order_release);
  if (config.experimental_mouse_fix) {
    g_logger.warn("ExperimentalMouseFix requested but disabled: run 20260615_230203 caused startup Fatal Error");
  }
  if (config.experimental_mouse_fix_v2) {
    if (experimental_mouse_fix_mode == 22U || experimental_mouse_fix_mode == 23U || experimental_mouse_fix_mode == 24U ||
        experimental_mouse_fix_mode == 25U || experimental_mouse_fix_mode == 26U || experimental_mouse_fix_mode == 27U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: gameplay-state-gated RInput-style cursor protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 21U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: RInput-style cursor protocol plus RMB key-state shim test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 20U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: Candidate-B site9 dual-dispatch RawInput payload replacement test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 19U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: Candidate-B site9 RawInput payload replacement test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 18U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style no-SetCursor-drain protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 17U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: synthetic RawInput RMB action activation test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 16U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style gameplay-center-armed cursor protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 15U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style gameplay-parent cursor protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 14U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style RMB or gameplay-LMB cursor protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 12U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style center-warp-gated cursor protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 11U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style RMB-gated cursor protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 10U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RInput-style GetCursorPos/SetCursorPos protocol test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 9U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: owned RawInput sink to Candidate-B test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 8U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RawInput-to-Candidate-B test with cursor observe hook, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 7U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RawInput-to-Candidate-B delta test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 6U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RawInput virtual-cursor hold test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 5U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed RawInput-to-live-cursor bridge test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 4U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: delayed live cursor parent 0x01DDC394 filter test, not a final Raw Input fix");
    } else if (experimental_mouse_fix_mode == 3U) {
      g_logger.warn("ExperimentalMouseFixV2 enabled: extra-dispatch 0x200 scale patch test, not a final Raw Input fix");
    } else {
      g_logger.warn("ExperimentalMouseFixV2 enabled: scale-only B-chain patch test, not a final Raw Input fix");
    }
  }
  const bool experimental_needs_cursor_hooks =
      experimental_mouse_fix_enabled && experimental_mouse_fix_mode == 2;
  g_observe_cursor_apis.store(config.observe_cursor_apis || experimental_needs_cursor_hooks, std::memory_order_release);
  if (config.observe_capture_apis) {
    g_logger.warn("ObserveCaptureApis requested but disabled: run 20260619_045906 caused startup Fatal Error before logs");
  }
  g_observe_capture_apis.store(false, std::memory_order_release);
  g_experimental_mouse_fix.store(experimental_mouse_fix_enabled, std::memory_order_release);
  g_experimental_mouse_fix_mode.store(experimental_mouse_fix_mode, std::memory_order_release);
  g_experimental_mouse_fix_max_age_ms.store(config.experimental_mouse_fix_max_age_ms, std::memory_order_release);
  g_experimental_mouse_fix_center_x.store(config.experimental_mouse_fix_center_x, std::memory_order_release);
  g_experimental_mouse_fix_center_y.store(config.experimental_mouse_fix_center_y, std::memory_order_release);
  g_experimental_mouse_fix_gain_x_bits.store(bits_from_float(config.experimental_mouse_fix_gain_x), std::memory_order_release);
  g_experimental_mouse_fix_gain_y_bits.store(bits_from_float(config.experimental_mouse_fix_gain_y), std::memory_order_release);
  g_candidate_b_cursor_qpc.store(0, std::memory_order_release);
  g_candidate_b_cursor_x.store(0, std::memory_order_release);
  g_candidate_b_cursor_y.store(0, std::memory_order_release);
  g_experimental_patch_applied.store(0, std::memory_order_release);
  g_experimental_patch_passthrough.store(0, std::memory_order_release);
  g_experimental_patch_stale.store(0, std::memory_order_release);
  g_experimental_patch_invalid.store(0, std::memory_order_release);
  g_experimental_synthetic_rmb_down.store(false, std::memory_order_release);
  g_experimental_synthetic_rmb_down_events.store(0, std::memory_order_release);
  g_experimental_synthetic_rmb_up_events.store(0, std::memory_order_release);
  g_experimental_key_state_forced.store(0, std::memory_order_release);
  g_experimental_site9_cached_qpc.store(0, std::memory_order_release);
  g_experimental_site9_cached_x.store(0, std::memory_order_release);
  g_experimental_site9_cached_y.store(0, std::memory_order_release);
  g_gameplay_state_last_center_warp_qpc.store(0, std::memory_order_release);
  g_gameplay_state_center_warp_burst_count.store(0, std::memory_order_release);
  g_gameplay_state_set_total.store(0, std::memory_order_release);
  g_gameplay_state_center_like.store(0, std::memory_order_release);
  g_gameplay_state_gameplay_like.store(0, std::memory_order_release);
  g_gameplay_state_menu_like.store(0, std::memory_order_release);
  g_gameplay_state_known_parent.store(0, std::memory_order_release);
  g_gameplay_state_first_gameplay_qpc.store(0, std::memory_order_release);
  g_gameplay_state_last_gameplay_qpc.store(0, std::memory_order_release);
  g_delayed_cursor_filter_active.store(false, std::memory_order_release);
  g_experimental_rinput_protocol_active.store(false, std::memory_order_release);
  g_experimental_last_center_warp_qpc.store(0, std::memory_order_release);
  g_experimental_center_warp_burst_count.store(0, std::memory_order_release);
  g_experimental_virtual_cursor_x.store(config.experimental_mouse_fix_center_x, std::memory_order_release);
  g_experimental_virtual_cursor_y.store(config.experimental_mouse_fix_center_y, std::memory_order_release);
  g_experimental_virtual_cursor_idle_calls.store(0, std::memory_order_release);
  g_stop_raw_input_sink.store(false, std::memory_order_release);
  g_raw_input_sink_hwnd.store(nullptr, std::memory_order_release);
  if (experimental_mouse_fix_enabled && (experimental_mouse_fix_mode == 26U || experimental_mouse_fix_mode == 27U)) {
    if (start_external_raw_input_companion()) {
      g_logger.warn("ExperimentalMouseFixV2 mode26 started external RawInput companion");
    } else {
      g_logger.warn("ExperimentalMouseFixV2 mode26 could not start external RawInput companion");
    }
    g_input_source_setting.store(config.input_source, std::memory_order_release);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_ingame_meter = std::make_unique<MouseRateMeter>(frequency.QuadPart);
    if (developer_mode) {
      g_ingame_csv.open(join_path(log_directory, L"ingame_input.csv"), "qpc,dx,dy,button_flags");
    }
    g_stop_ingame_hook.store(false, std::memory_order_release);
    g_ingame_hook_thread = std::thread(ingame_hook_loop);
    g_sink_thread = std::thread(sink_loop);
    g_logger.info("InputSource=" + std::to_string(config.input_source) + " (0=auto, 1=game, 2=companion)");
  }
  if (config.observe_internal_site12) {
    g_logger.warn("ObserveInternalSite12 requested but disabled: run 20260615_130241 caused startup Fatal Error");
  }
  if (config.observe_internal_site13) {
    g_logger.warn("ObserveInternalSite13 requested but disabled: run 20260615_174346 caused startup Fatal Error");
  }
  g_observe_internal_site12.store(false, std::memory_order_release);
  g_observe_internal_site13.store(false, std::memory_order_release);
  g_logger.info(std::string("ObserveRawInput=") + (config.observe_raw_input ? "true" : "false") +
                " ObserveGetProcAddress=" + (config.observe_get_proc_address ? "true" : "false") +
                " ObserveCursorApis=" + (config.observe_cursor_apis ? "true" : "false") +
                " ObserveCaptureApis=false" +
                " ObserveTiming=" + (config.observe_timing ? "true" : "false") +
                " ObserveInternalCandidateA=" + (config.observe_internal_candidate_a ? "true" : "false") +
                " ObserveInternalSite12=false" +
                " ObserveInternalSite13=false" +
                " ObserveMarkers=" + (config.observe_markers ? "true" : "false") +
                " ObserveGameplayState=" +
                ((config.observe_gameplay_state ||
                  (experimental_mouse_fix_enabled &&
                   (experimental_mouse_fix_mode == 22U || experimental_mouse_fix_mode == 23U ||
                    experimental_mouse_fix_mode == 24U || experimental_mouse_fix_mode == 25U ||
                    experimental_mouse_fix_mode == 26U || experimental_mouse_fix_mode == 27U)))
                     ? "true"
                     : "false") +
                " ExperimentalMouseFix=false" +
                " ExperimentalMouseFixV2=" + (config.experimental_mouse_fix_v2 ? "true" : "false") +
                " ForceUnlimitedFps=" + (config.force_unlimited_fps ? "true" : "false") +
                " ExperimentalMouseFixMode=" + std::to_string(experimental_mouse_fix_mode) +
                " ExperimentalMouseFixGainX=" + std::to_string(config.experimental_mouse_fix_gain_x) +
                " ExperimentalMouseFixGainY=" + std::to_string(config.experimental_mouse_fix_gain_y) +
                " RingCapacity=" + std::to_string(config.ring_capacity));
  auto* user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    user32 = LoadLibraryW(L"user32.dll");
  }
  g_observer->set_originals(
      user32 ? reinterpret_cast<RawInputObserver::GetRawInputDataFn>(GetProcAddress(user32, "GetRawInputData")) : nullptr,
      user32 ? reinterpret_cast<RawInputObserver::GetRawInputBufferFn>(GetProcAddress(user32, "GetRawInputBuffer")) : nullptr,
      user32 ? reinterpret_cast<RawInputObserver::RegisterRawInputDevicesFn>(GetProcAddress(user32, "RegisterRawInputDevices"))
             : nullptr);
  if (experimental_mouse_fix_mode == 17U) {
    g_observer->set_mouse_mutator(experimental_mutate_raw_mouse);
  }
  if (experimental_mouse_fix_enabled && (experimental_mouse_fix_mode == 9U || experimental_mouse_fix_mode == 10U ||
                                         experimental_mouse_fix_mode == 11U || experimental_mouse_fix_mode == 12U ||
                                         experimental_mouse_fix_mode == 14U || experimental_mouse_fix_mode == 15U ||
                                         experimental_mouse_fix_mode == 16U || experimental_mouse_fix_mode == 17U ||
                                         experimental_mouse_fix_mode == 18U || experimental_mouse_fix_mode == 19U ||
                                         experimental_mouse_fix_mode == 20U || experimental_mouse_fix_mode == 21U ||
                                         experimental_mouse_fix_mode == 22U || experimental_mouse_fix_mode == 23U ||
                                         experimental_mouse_fix_mode == 24U)) {
    g_raw_input_sink_thread = std::thread(raw_input_sink_loop);
    g_logger.info("Started owned RawInput sink thread");
  }

  auto* kernel32 = GetModuleHandleW(L"kernel32.dll");
  g_original_get_proc_address.store(
      kernel32 ? reinterpret_cast<GetProcAddressFn>(GetProcAddress(kernel32, "GetProcAddress")) : nullptr,
      std::memory_order_release);
  g_original_get_cursor_pos.store(
      user32 ? reinterpret_cast<GetCursorPosFn>(GetProcAddress(user32, "GetCursorPos")) : nullptr,
      std::memory_order_release);
  g_original_set_cursor_pos.store(
      user32 ? reinterpret_cast<SetCursorPosFn>(GetProcAddress(user32, "SetCursorPos")) : nullptr,
      std::memory_order_release);
  g_original_get_async_key_state.store(
      user32 ? reinterpret_cast<GetAsyncKeyStateFn>(GetProcAddress(user32, "GetAsyncKeyState")) : nullptr,
      std::memory_order_release);
  g_original_get_key_state.store(
      user32 ? reinterpret_cast<GetKeyStateFn>(GetProcAddress(user32, "GetKeyState")) : nullptr,
      std::memory_order_release);
  g_original_clip_cursor.store(
      user32 ? reinterpret_cast<ClipCursorFn>(GetProcAddress(user32, "ClipCursor")) : nullptr,
      std::memory_order_release);
  g_original_get_clip_cursor.store(
      user32 ? reinterpret_cast<GetClipCursorFn>(GetProcAddress(user32, "GetClipCursor")) : nullptr,
      std::memory_order_release);

  if (config.observe_raw_input) {
    const auto data_hooks = g_get_raw_input_data_hooks.install_main_module(
        "USER32.dll", "GetRawInputData", reinterpret_cast<void*>(&observed_get_raw_input_data));
    const auto buffer_hooks = g_get_raw_input_buffer_hooks.install_main_module(
        "USER32.dll", "GetRawInputBuffer", reinterpret_cast<void*>(&observed_get_raw_input_buffer));
    const auto register_hooks = g_register_raw_input_devices_hooks.install_main_module(
        "USER32.dll", "RegisterRawInputDevices", reinterpret_cast<void*>(&observed_register_raw_input_devices));
    std::size_t getproc_hooks = 0;
    if (config.observe_get_proc_address) {
      getproc_hooks = g_get_proc_address_hooks.install_main_module(
          "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&observed_get_proc_address), self_module);
    }
    std::size_t get_cursor_hooks = 0;
    std::size_t set_cursor_hooks = 0;
    std::size_t clip_cursor_hooks = 0;
    std::size_t get_clip_cursor_hooks = 0;
    if (config.observe_cursor_apis || experimental_needs_cursor_hooks) {
      get_cursor_hooks =
          g_get_cursor_pos_hooks.install_main_module("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&observed_get_cursor_pos));
      set_cursor_hooks =
          g_set_cursor_pos_hooks.install_main_module("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&observed_set_cursor_pos));
      clip_cursor_hooks =
          g_clip_cursor_hooks.install_main_module("USER32.dll", "ClipCursor", reinterpret_cast<void*>(&observed_clip_cursor));
      get_clip_cursor_hooks = g_get_clip_cursor_hooks.install_main_module(
          "USER32.dll", "GetClipCursor", reinterpret_cast<void*>(&observed_get_clip_cursor));
    }
    g_logger.info("Installed ObserveOnly IAT wrappers: GetRawInputData=" + std::to_string(data_hooks) +
                  " GetRawInputBuffer=" + std::to_string(buffer_hooks) +
                  " RegisterRawInputDevices=" + std::to_string(register_hooks) +
                  " GetProcAddress=" + std::to_string(getproc_hooks) +
                  " GetCursorPos=" + std::to_string(get_cursor_hooks) +
                  " SetCursorPos=" + std::to_string(set_cursor_hooks) +
                  " ClipCursor=" + std::to_string(clip_cursor_hooks) +
                  " GetClipCursor=" + std::to_string(get_clip_cursor_hooks));
  }

  if (experimental_mouse_fix_enabled && (experimental_mouse_fix_mode == 26U || experimental_mouse_fix_mode == 27U)) {
    // Needed in every mode: the in-process sink must know when the game takes or drops the mouse.
    const auto register_hooks = g_register_raw_input_devices_hooks.install_main_module(
        "USER32.dll", "RegisterRawInputDevices", reinterpret_cast<void*>(&observed_register_raw_input_devices));
    g_logger.info("RegisterRawInputDevices hook installed=" + std::to_string(register_hooks) +
                  " (total " + std::to_string(g_register_raw_input_devices_hooks.count()) + ")");
  }

  if (config.observe_timing) {
    g_stop_timing.store(false, std::memory_order_release);
    g_timing_thread = std::thread(timing_loop);
    g_logger.info("Started timing observer thread");
  }

  if (config.observe_internal_candidate_a || experimental_mouse_fix_enabled || config.observe_gameplay_state) {
    if (!game_fingerprint) {
      g_logger.warn("Candidate A trace disabled: executable fingerprint was unavailable");
    } else {
      g_internal_trace_read.store(0, std::memory_order_release);
      g_internal_trace_write.store(0, std::memory_order_release);
      g_internal_trace_dropped.store(0, std::memory_order_release);
      g_internal_trace_hits.store(0, std::memory_order_release);
      g_internal_trace_max_rows.store(config.internal_trace_max_rows, std::memory_order_release);
      g_stop_internal_trace.store(false, std::memory_order_release);
      const auto internal_header =
          std::string("sequence,qpc,thread_id,rva,site,segment_id,segment_label,delta_x,delta_y,ref_x,ref_y,current_x,current_y,pre_store_x,pre_store_y,memory_ok,arg_memory_ok,stack_arg_memory_ok") +
          argument_snapshot_header("rdx_f") + argument_snapshot_header("r8_f") + argument_snapshot_header("r9_f") +
          argument_snapshot_header("stack20_f") +
          ",rax,rbx,rcx,rdx,r8,r9,r10,r11,r12,r13,r14,r15,call_target,stack_arg20,stack_arg28,rsp,rbp";
      if (developer_mode) {
        g_internal_a_csv.open(internal_a_csv_path, internal_header);
      }
      g_internal_trace_thread = std::thread(internal_trace_loop, *game_fingerprint, config.internal_trace_delay_ms);
      if (config.observe_gameplay_state && !experimental_mouse_fix_enabled && !config.observe_internal_candidate_a) {
        g_logger.info("GameplayStateProbe SetCursorPos-only observer scheduled after " +
                      std::to_string(config.internal_trace_delay_ms) + " ms");
      } else {
        g_logger.info(std::string(experimental_mouse_fix_enabled ? "Candidate A/B internal experimental patch scheduled after "
                                                                 : "Candidate A internal ObserveOnly trace scheduled after ") +
                      std::to_string(config.internal_trace_delay_ms) + " ms, max rows=" +
                      std::to_string(config.internal_trace_max_rows));
      }
    }
  }

  running_ = true;
  g_runtime_running.store(true, std::memory_order_release);
  g_logger.info("GTASADE.PCFix ObserveOnly started");
  return true;
}

void ObserveOnlyRuntime::stop() {
  if (!running_) {
    return;
  }

  g_logger.info("GTASADE.PCFix ObserveOnly stopping");
  if (g_external_raw_shared != nullptr) {
    g_logger.info("ExternalRawInput packets=" +
                  std::to_string(InterlockedCompareExchange64(&g_external_raw_shared->packet_count, 0, 0)) +
                  " last_qpc=" + std::to_string(InterlockedCompareExchange64(&g_external_raw_shared->last_qpc, 0, 0)));
  }
  g_fps_lock.stop();
  g_stop_ingame_hook.store(true, std::memory_order_release);
  if (g_ingame_hook_thread.joinable()) {
    g_ingame_hook_thread.join();
  }
  stop_sink();
  g_logger.info("InGameRawInput packets=" + std::to_string(g_ingame_packets.load()) +
                " sink_registrations=" + std::to_string(g_sink_registrations.load()));
  stop_external_raw_input_companion();
  g_stop_markers.store(true, std::memory_order_release);
  if (g_marker_thread.joinable()) {
    g_marker_thread.join();
  }
  g_stop_raw_input_sink.store(true, std::memory_order_release);
  if (const auto hwnd = g_raw_input_sink_hwnd.load(std::memory_order_acquire); hwnd != nullptr) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
  if (g_raw_input_sink_thread.joinable()) {
    g_raw_input_sink_thread.join();
  }
  g_stop_internal_trace.store(true, std::memory_order_release);
  uninstall_internal_trace();
  if (g_internal_trace_thread.joinable()) {
    g_internal_trace_thread.join();
  }
  flush_internal_trace();
  g_stop_timing.store(true, std::memory_order_release);
  if (g_timing_thread.joinable()) {
    g_timing_thread.join();
  }
  g_get_raw_input_data_hooks.uninstall();
  g_get_raw_input_buffer_hooks.uninstall();
  g_register_raw_input_devices_hooks.uninstall();
  g_get_proc_address_hooks.uninstall();
  g_get_cursor_pos_hooks.uninstall();
  g_set_cursor_pos_hooks.uninstall();
  g_get_async_key_state_hooks.uninstall();
  g_get_key_state_hooks.uninstall();
  g_clip_cursor_hooks.uninstall();
  g_get_clip_cursor_hooks.uninstall();
  if (g_queue) {
    const auto stats = g_queue->statistics();
    g_logger.info("RawInput observed=" + std::to_string(stats.observed) +
                  " buffered_observed=" + std::to_string(stats.buffered_observed) +
                  " duplicates=" + std::to_string(stats.duplicates) + " size_queries=" + std::to_string(stats.size_queries) +
                  " buffer_size_queries=" + std::to_string(stats.buffer_size_queries) +
                  " non_mouse=" + std::to_string(stats.non_mouse) + " errors=" + std::to_string(stats.errors) +
                  " buffer_errors=" + std::to_string(stats.buffer_errors) +
                  " registrations=" + std::to_string(stats.registrations) +
                  " wm_input_messages=" + std::to_string(stats.wm_input_messages) +
                  " overflow=" + std::to_string(stats.overflow) + " accumulated_x=" + std::to_string(stats.accumulated_x) +
                  " accumulated_y=" + std::to_string(stats.accumulated_y));
  }
  if (g_experimental_mouse_fix.load(std::memory_order_acquire)) {
    g_logger.info("ExperimentalMouseFix applied=" +
                  std::to_string(g_experimental_patch_applied.load(std::memory_order_acquire)) +
                  " passthrough=" +
                  std::to_string(g_experimental_patch_passthrough.load(std::memory_order_acquire)) +
                  " stale=" + std::to_string(g_experimental_patch_stale.load(std::memory_order_acquire)) +
                  " invalid=" + std::to_string(g_experimental_patch_invalid.load(std::memory_order_acquire)) +
                  " center_x=" + std::to_string(g_experimental_mouse_fix_center_x.load(std::memory_order_acquire)) +
                  " center_y=" + std::to_string(g_experimental_mouse_fix_center_y.load(std::memory_order_acquire)) +
                  " synthetic_rmb_down=" +
                  std::to_string(g_experimental_synthetic_rmb_down_events.load(std::memory_order_acquire)) +
                  " synthetic_rmb_up=" +
                  std::to_string(g_experimental_synthetic_rmb_up_events.load(std::memory_order_acquire)) +
                  " key_state_forced=" +
                  std::to_string(g_experimental_key_state_forced.load(std::memory_order_acquire)));
    log_cursor_protocol_stats();
  }
  if (g_observe_gameplay_state.load(std::memory_order_acquire)) {
    g_logger.info("GameplayStateProbe set_total=" +
                  std::to_string(g_gameplay_state_set_total.load(std::memory_order_acquire)) +
                  " center_like=" +
                  std::to_string(g_gameplay_state_center_like.load(std::memory_order_acquire)) +
                  " known_parent=" +
                  std::to_string(g_gameplay_state_known_parent.load(std::memory_order_acquire)) +
                  " gameplay_like=" +
                  std::to_string(g_gameplay_state_gameplay_like.load(std::memory_order_acquire)) +
                  " menu_like=" +
                  std::to_string(g_gameplay_state_menu_like.load(std::memory_order_acquire)) +
                  " burst_count=" +
                  std::to_string(g_gameplay_state_center_warp_burst_count.load(std::memory_order_acquire)) +
                  " first_gameplay_qpc=" +
                  std::to_string(g_gameplay_state_first_gameplay_qpc.load(std::memory_order_acquire)) +
                  " last_gameplay_qpc=" +
                  std::to_string(g_gameplay_state_last_gameplay_qpc.load(std::memory_order_acquire)));
    log_cursor_protocol_stats();
  }
  g_observer.reset();
  g_queue.reset();
  g_stop_trace_flush.store(true, std::memory_order_release);
  if (g_trace_flush_thread.joinable()) {
    g_trace_flush_thread.join();
  }
  g_cursor_csv.close();
  g_frames_csv.close();
  g_ingame_csv.close();
  g_raw_csv.close();
  g_timing_csv.close();
  g_internal_a_csv.close();
  g_marker_csv.close();
  g_logger.info("GTASADE.PCFix ObserveOnly stopped");
  g_logger.close();
  g_runtime_running.store(false, std::memory_order_release);
  if (g_instance_mutex != nullptr) {
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
  }
  running_ = false;
}

}  // namespace sade

namespace {

void copy_text(char* destination, std::size_t capacity, const std::string& text) {
  if (capacity == 0) {
    return;
  }
  const auto count = std::min(capacity - 1, text.size());
  std::memcpy(destination, text.data(), count);
  destination[count] = '\0';
}

std::string utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
  if (bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
  }
  return out;
}

bool write_ini_value(const std::wstring& path, const wchar_t* key, const std::wstring& value) {
  return WritePrivateProfileStringW(L"Mouse", key, value.c_str(), path.c_str()) != FALSE;
}

}  // namespace

extern "C" __declspec(dllexport) bool SadeGetStatus(sade::api::StatusV1* status) {
  if (status == nullptr || status->size < sizeof(sade::api::StatusV1)) {
    return false;
  }
  using namespace sade;
  status->running = g_runtime_running.load(std::memory_order_acquire) ? 1 : 0;
  status->enabled = g_user_enabled.load(std::memory_order_acquire) ? 1 : 0;
  status->supported_build = g_supported_build.load(std::memory_order_acquire) ? 1 : 0;
  status->mode = static_cast<std::int32_t>(g_experimental_mouse_fix_mode.load(std::memory_order_acquire));
  status->hooks_active = g_experimental_rinput_protocol_active.load(std::memory_order_acquire) ? 1 : 0;
  status->companion_running = 0;
  if (g_external_raw_process != nullptr) {
    status->companion_running = WaitForSingleObject(g_external_raw_process, 0) == WAIT_TIMEOUT ? 1 : 0;
  }
  status->gameplay_active = gameplay_state_recent_active() ? 1 : 0;
  status->gain_x = float_from_bits(g_experimental_mouse_fix_gain_x_bits.load(std::memory_order_acquire));
  status->gain_y = float_from_bits(g_experimental_mouse_fix_gain_y_bits.load(std::memory_order_acquire));
  status->companion_packets =
      g_external_raw_shared != nullptr ? InterlockedCompareExchange64(&g_external_raw_shared->packet_count, 0, 0) : 0;
  status->applied = g_experimental_patch_applied.load(std::memory_order_relaxed);
  status->passthrough = g_experimental_patch_passthrough.load(std::memory_order_relaxed);
  status->stale = g_experimental_patch_stale.load(std::memory_order_relaxed);
  copy_text(status->core_version, sizeof(status->core_version), SADE_CORE_VERSION);

  if (g_external_raw_shared != nullptr) {
    auto* shared = g_external_raw_shared;
    status->polling_hz = InterlockedCompareExchange(&shared->polling_hz, 0, 0);
    status->packets_last_second = InterlockedCompareExchange(&shared->packets_last_second, 0, 0);
    status->max_gap_us_last_second = InterlockedCompareExchange(&shared->max_gap_us_last_second, 0, 0);
    // Seqlock-style read: retry if the companion rewrote the name while we copied it.
    for (int attempt = 0; attempt < 4; ++attempt) {
      const LONG before = InterlockedCompareExchange(&shared->device_generation, 0, 0);
      if ((before & 1) != 0) {
        continue;
      }
      wchar_t name[kExternalRawInputDeviceNameChars]{};
      std::memcpy(name, const_cast<const wchar_t*>(shared->device_name), sizeof(name));
      name[kExternalRawInputDeviceNameChars - 1] = L'\0';
      if (InterlockedCompareExchange(&shared->device_generation, 0, 0) == before) {
        copy_text(status->device_name, sizeof(status->device_name), utf8(name));
        break;
      }
    }
  }

  status->sink_registered = g_sink_registered.load(std::memory_order_relaxed) ? 1 : 0;
  status->recenter_interval_us = static_cast<std::int32_t>(g_recenter_interval_ticks.load(std::memory_order_relaxed) *
                                                           1000.0 / static_cast<double>(std::max<LONGLONG>(qpc_ticks_for_ms(1), 1)));
  status->max_fps = g_fps_lock.target();
  status->fps_lock_objects = g_fps_lock.objects();
  status->game_fps_setting = g_fps_lock.game_value();
  status->ingame_hook_installed = g_ingame_hook != nullptr ? 1 : 0;
  status->ingame_packets = g_ingame_packets.load(std::memory_order_relaxed);
  status->companion_polling_hz = status->polling_hz;
  status->input_source = 2;
  if (g_external_raw_shared != nullptr &&
      ingame_source_live(InterlockedCompareExchange64(&g_external_raw_shared->last_qpc, 0, 0))) {
    status->input_source = 1;
    status->polling_hz = g_ingame_polling_hz.load(std::memory_order_relaxed);
    status->packets_last_second = g_ingame_packets_last_second.load(std::memory_order_relaxed);
    status->max_gap_us_last_second = g_ingame_max_gap_us.load(std::memory_order_relaxed);
  }
  status->developer_mode_active = g_developer_mode_active.load(std::memory_order_acquire) ? 1 : 0;
  status->developer_mode_configured = g_developer_mode_configured.load(std::memory_order_acquire) ? 1 : 0;
  std::lock_guard lock(g_settings_mutex);
  copy_text(status->game_version, sizeof(status->game_version), g_game_version);
  copy_text(status->developer_log_directory, sizeof(status->developer_log_directory), utf8(g_developer_log_directory));
  copy_text(status->developer_run_directory, sizeof(status->developer_run_directory), utf8(g_developer_run_directory));
  return true;
}

extern "C" __declspec(dllexport) bool SadeSetDeveloperMode(bool enabled, const wchar_t* log_directory) {
  using namespace sade;
  std::lock_guard lock(g_settings_mutex);
  if (g_ini_path.empty()) {
    return false;
  }
  if (log_directory != nullptr) {
    g_developer_log_directory = log_directory;
  }
  g_developer_mode_configured.store(enabled, std::memory_order_release);
  const bool ok_enabled =
      WritePrivateProfileStringW(L"Developer", L"Enabled", enabled ? L"1" : L"0", g_ini_path.c_str()) != FALSE;
  const bool ok_directory = WritePrivateProfileStringW(L"Developer", L"LogDirectory", g_developer_log_directory.c_str(),
                                                       g_ini_path.c_str()) != FALSE;
  return ok_enabled && ok_directory;
}

extern "C" __declspec(dllexport) void SadeDevFrame() {
  using namespace sade;
  const auto frame = g_frame_index.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!g_frames_csv.enabled()) {
    return;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  char row[64]{};
  const int length = std::snprintf(row, sizeof(row), "%lld,%llu", static_cast<long long>(now.QuadPart),
                                   static_cast<unsigned long long>(frame));
  if (length > 0) {
    g_frames_csv.write_row(row, static_cast<std::size_t>(length));
  }
}

extern "C" __declspec(dllexport) void SadeSetEnabled(bool enabled) {
  sade::g_user_enabled.store(enabled, std::memory_order_release);
}

extern "C" __declspec(dllexport) void SadeSetGain(float gain_x, float gain_y) {
  using namespace sade;
  if (!std::isfinite(gain_x) || !std::isfinite(gain_y)) {
    return;
  }
  gain_x = std::clamp(gain_x, 0.05F, 10.0F);
  gain_y = std::clamp(gain_y, 0.05F, 10.0F);
  g_experimental_mouse_fix_gain_x_bits.store(bits_from_float(gain_x), std::memory_order_release);
  g_experimental_mouse_fix_gain_y_bits.store(bits_from_float(gain_y), std::memory_order_release);
}

extern "C" __declspec(dllexport) bool SadeSaveSettings() {
  using namespace sade;
  std::lock_guard lock(g_settings_mutex);
  if (g_ini_path.empty()) {
    return false;
  }
  wchar_t gain_x[32]{};
  wchar_t gain_y[32]{};
  swprintf_s(gain_x, L"%.3f", static_cast<double>(float_from_bits(g_experimental_mouse_fix_gain_x_bits.load())));
  swprintf_s(gain_y, L"%.3f", static_cast<double>(float_from_bits(g_experimental_mouse_fix_gain_y_bits.load())));
  bool ok = write_ini_value(g_ini_path, L"Enabled", g_user_enabled.load() ? L"1" : L"0");
  ok = write_ini_value(g_ini_path, L"SensitivityX", gain_x) && ok;
  ok = write_ini_value(g_ini_path, L"SensitivityY", gain_y) && ok;
  return ok;
}

extern "C" __declspec(dllexport) bool SadeSetMaxFps(std::int32_t max_fps) {
  using namespace sade;
  max_fps = std::clamp(max_fps, -1, 1000);
  g_fps_lock.set_target(max_fps);
  std::lock_guard lock(g_settings_mutex);
  if (g_ini_path.empty()) {
    return false;
  }
  const bool ok = WritePrivateProfileStringW(L"Game", L"MaxFps", std::to_wstring(max_fps).c_str(), g_ini_path.c_str()) != FALSE;
  return ok;
}

namespace {
sade::ValueScanner g_value_scanner;
double g_last_scan_value = 0.0;
}  // namespace

extern "C" __declspec(dllexport) bool SadeScanStart(double value, bool first) {
  const bool started = g_value_scanner.start_scan(value, first);
  if (started) {
    g_last_scan_value = value;
    sade::g_logger.info(std::string("ValueScanner ") + (first ? "first" : "next") + " scan for " + std::to_string(value));
  }
  return started;
}

extern "C" __declspec(dllexport) bool SadeScanStatus(sade::api::ScanStatusV1* status) {
  if (status == nullptr || status->size < sizeof(sade::api::ScanStatusV1)) {
    return false;
  }
  const auto s = g_value_scanner.status();
  status->busy = s.busy ? 1 : 0;
  status->scans_done = s.scans_done;
  status->int_candidates = s.counts[sade::ValueScanner::Int32];
  status->float_candidates = s.counts[sade::ValueScanner::Float];
  status->double_candidates = s.counts[sade::ValueScanner::Double];
  status->bytes_scanned = s.bytes_scanned;
  status->last_scan_seconds = s.last_scan_seconds;
  return true;
}

extern "C" __declspec(dllexport) std::int32_t SadeScanWrite(double value) {
  const int written = g_value_scanner.write_all(value, 64);
  sade::g_logger.info("ValueScanner wrote " + std::to_string(value) + " to " + std::to_string(written) + " candidates");
  return written;
}

extern "C" __declspec(dllexport) std::int32_t SadeScanDump() {
  using namespace sade;
  const auto list = g_value_scanner.candidates(64);
  const auto base = g_game_image_base.load();
  const auto end = g_game_image_end.load();
  g_logger.info("ValueScanner dump (" + std::to_string(list.size()) + " candidates, last value " +
                std::to_string(g_last_scan_value) + ")");
  for (const auto& candidate : list) {
    g_logger.info(describe_candidate(candidate, base, end));
  }
  return static_cast<std::int32_t>(list.size());
}
