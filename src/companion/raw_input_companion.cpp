#include "input/external_raw_input_shared.h"
#include "input/mouse_rate_meter.h"

#include <Windows.h>
#include <hidsdi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>

namespace {

sade::ExternalRawInputShared* g_shared = nullptr;
LARGE_INTEGER g_qpc_frequency{};

// Optional per-packet trace (developer mode). Written from this process, so it never stalls the game.
HANDLE g_log_file = INVALID_HANDLE_VALUE;
std::string g_log_buffer;

sade::MouseRateMeter* g_rate_meter = nullptr;
HANDLE g_current_device = nullptr;
bool g_device_known = false;

void flush_log() {
  if (g_log_file == INVALID_HANDLE_VALUE || g_log_buffer.empty()) {
    return;
  }
  DWORD written = 0;
  WriteFile(g_log_file, g_log_buffer.data(), static_cast<DWORD>(g_log_buffer.size()), &written, nullptr);
  g_log_buffer.clear();
}

void publish_device_name(HANDLE device) {
  if (g_device_known && device == g_current_device) {
    return;
  }
  g_device_known = true;
  g_current_device = device;

  std::wstring name;
  wchar_t path[512]{};
  UINT path_chars = static_cast<UINT>(std::size(path));
  if (device != nullptr && GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path, &path_chars) > 0) {
    HANDLE hid = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hid != INVALID_HANDLE_VALUE) {
      wchar_t manufacturer[128]{};
      wchar_t product[128]{};
      if (HidD_GetManufacturerString(hid, manufacturer, sizeof(manufacturer)) && manufacturer[0] != L'\0') {
        name = manufacturer;
      }
      if (HidD_GetProductString(hid, product, sizeof(product)) && product[0] != L'\0') {
        name += name.empty() ? L"" : L" ";
        name += product;
      }
      CloseHandle(hid);
    }
    if (name.empty()) {
      // Fall back to VID/PID from a path like \\?\HID#VID_046D&PID_C547&MI_01#...
      const std::wstring_view view(path);
      const auto vid = view.find(L"VID_");
      const auto pid = view.find(L"PID_");
      if (vid != std::wstring_view::npos && pid != std::wstring_view::npos) {
        name = L"VID " + std::wstring(view.substr(vid + 4, 4)) + L" PID " + std::wstring(view.substr(pid + 4, 4));
      }
    }
  }
  if (name.empty()) {
    name = device == nullptr ? L"Injected input (no device)" : L"Unknown mouse";
  }

  InterlockedIncrement(&g_shared->device_generation);  // Odd: writing.
  wcsncpy_s(g_shared->device_name, name.c_str(), _TRUNCATE);
  InterlockedIncrement(&g_shared->device_generation);  // Even: stable.
}

void record_packet(const RAWINPUT& raw, LONGLONG now) {
  sade::MouseRateMeter::Snapshot snapshot{};
  if (g_rate_meter != nullptr && g_rate_meter->record(now, snapshot)) {
    InterlockedExchange(&g_shared->polling_hz, snapshot.polling_hz);
    InterlockedExchange(&g_shared->packets_last_second, snapshot.packets_last_second);
    InterlockedExchange(&g_shared->max_gap_us_last_second, snapshot.max_gap_us_last_second);
  }

  if (g_log_file != INVALID_HANDLE_VALUE) {
    char row[160]{};
    const int length = std::snprintf(row, sizeof(row), "%lld,%ld,%ld,%u,%u,%llu\r\n", static_cast<long long>(now),
                                     raw.data.mouse.lLastX, raw.data.mouse.lLastY, raw.data.mouse.usFlags,
                                     raw.data.mouse.usButtonFlags,
                                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(raw.header.hDevice)));
    if (length > 0) {
      g_log_buffer.append(row, static_cast<std::size_t>(length));
    }
    if (g_log_buffer.size() >= 256 * 1024) {
      flush_log();
    }
  }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_INPUT && g_shared != nullptr) {
    alignas(8) std::array<std::byte, 256> buffer{};
    UINT size = static_cast<UINT>(buffer.size());
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) !=
            static_cast<UINT>(-1) &&
        size >= sizeof(RAWINPUTHEADER)) {
      const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
      if (raw->header.dwType == RIM_TYPEMOUSE) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // Deltas first: this is the only data the game-side hook depends on.
        if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
          InterlockedAdd64(&g_shared->pending_x, raw->data.mouse.lLastX);
          InterlockedAdd64(&g_shared->pending_y, raw->data.mouse.lLastY);
        }
        InterlockedExchange64(&g_shared->last_qpc, now.QuadPart);
        InterlockedIncrement64(&g_shared->packet_count);
        publish_device_name(raw->header.hDevice);
        record_packet(*raw, now.QuadPart);
      }
    }
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool parse_arg(const wchar_t* name, int argc, wchar_t** argv, std::wstring& value) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) == name) {
      value = argv[index + 1];
      return true;
    }
  }
  return false;
}

// Keep input latency stable: Windows 11 throttles (EcoQoS) background processes without a window.
void raise_input_priority() {
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return 2;
  }

  std::wstring mapping_name;
  std::wstring stop_event_name;
  std::wstring log_path;
  const bool valid_args = parse_arg(L"--mapping", argc, argv, mapping_name) &&
                          parse_arg(L"--stop-event", argc, argv, stop_event_name);
  parse_arg(L"--log", argc, argv, log_path);
  LocalFree(argv);
  if (!valid_args) {
    return 2;
  }

  QueryPerformanceFrequency(&g_qpc_frequency);
  sade::MouseRateMeter rate_meter(g_qpc_frequency.QuadPart);
  g_rate_meter = &rate_meter;
  raise_input_priority();

  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str());
  if (mapping == nullptr) {
    return 3;
  }
  auto* shared = static_cast<sade::ExternalRawInputShared*>(
      MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(sade::ExternalRawInputShared)));
  if (shared == nullptr || shared->magic != sade::kExternalRawInputMagic ||
      shared->version != sade::kExternalRawInputVersion) {
    if (shared != nullptr) {
      UnmapViewOfFile(shared);
    }
    CloseHandle(mapping);
    return 4;
  }

  HANDLE stop_event = OpenEventW(SYNCHRONIZE, FALSE, stop_event_name.c_str());
  if (stop_event == nullptr) {
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 5;
  }

  if (!log_path.empty()) {
    g_log_file =
        CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_log_buffer.reserve(300 * 1024);
    g_log_buffer = "qpc,dx,dy,flags,button_flags,device\r\n";
  }

  constexpr wchar_t kWindowClass[] = L"GTASADE.PCFix.ExternalRawInput";
  WNDCLASSW window_class{};
  window_class.hInstance = instance;
  window_class.lpfnWndProc = window_proc;
  window_class.lpszClassName = kWindowClass;
  RegisterClassW(&window_class);
  HWND window = CreateWindowExW(0, kWindowClass, kWindowClass, 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    CloseHandle(stop_event);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 6;
  }

  g_shared = shared;
  RAWINPUTDEVICE device{};
  device.usUsagePage = 0x01;
  device.usUsage = 0x02;
  device.dwFlags = RIDEV_INPUTSINK;
  device.hwndTarget = window;
  if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
    DestroyWindow(window);
    CloseHandle(stop_event);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 7;
  }

  bool running = true;
  while (running) {
    const DWORD wait = MsgWaitForMultipleObjects(1, &stop_event, FALSE, 1000, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait == WAIT_TIMEOUT) {
      flush_log();  // Mouse idle: a good moment to write.
      continue;
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  g_shared = nullptr;
  flush_log();
  if (g_log_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_log_file);
  }
  DestroyWindow(window);
  CloseHandle(stop_event);
  UnmapViewOfFile(shared);
  CloseHandle(mapping);
  return 0;
}
