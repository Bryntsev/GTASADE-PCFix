#include "input/raw_input_observer.h"

#include <cstddef>
#include <sstream>

namespace sade {
namespace {

std::int64_t qpc_now() {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

}  // namespace

RawInputObserver::RawInputObserver(RawInputQueue& queue, CsvTrace& csv) : queue_(queue), csv_(csv) {}

void RawInputObserver::set_originals(GetRawInputDataFn data,
                                     GetRawInputBufferFn buffer,
                                     RegisterRawInputDevicesFn register_devices) {
  original_data_.store(data, std::memory_order_release);
  original_buffer_.store(buffer, std::memory_order_release);
  original_register_devices_.store(register_devices, std::memory_order_release);
}

void RawInputObserver::set_mouse_mutator(RawMouseMutatorFn mutator) {
  mouse_mutator_.store(mutator, std::memory_order_release);
}

RawInputObserver::GetRawInputDataFn RawInputObserver::original_data() const {
  return original_data_.load(std::memory_order_acquire);
}

RawInputObserver::GetRawInputBufferFn RawInputObserver::original_buffer() const {
  return original_buffer_.load(std::memory_order_acquire);
}

RawInputObserver::RegisterRawInputDevicesFn RawInputObserver::original_register_devices() const {
  return original_register_devices_.load(std::memory_order_acquire);
}

UINT RawInputObserver::observe_data(
    HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size, std::uintptr_t caller_rva) {
  const auto fn = original_data();
  if (fn == nullptr) {
    return static_cast<UINT>(-1);
  }

  const UINT result = fn(input, command, data, size, header_size);
  if (result == static_cast<UINT>(-1)) {
    queue_.record_error();
    return result;
  }
  if (command == RID_INPUT && data == nullptr) {
    queue_.record_size_query();
    return result;
  }
  if (command == RID_INPUT && data != nullptr && result >= sizeof(RAWINPUTHEADER)) {
    auto* raw = static_cast<RAWINPUT*>(data);
    if (raw->header.dwType != RIM_TYPEMOUSE) {
      queue_.record_non_mouse();
      return result;
    }
    if (const auto mutator = mouse_mutator_.load(std::memory_order_acquire); mutator != nullptr) {
      mutator(raw->data.mouse);
    }
    observe_mouse("data", input, command, *raw, result, caller_rva);
  }
  return result;
}

UINT RawInputObserver::observe_buffer(PRAWINPUT data, PUINT size, UINT header_size) {
  const auto fn = original_buffer();
  if (fn == nullptr) {
    return static_cast<UINT>(-1);
  }

  const UINT result = fn(data, size, header_size);
  if (result == static_cast<UINT>(-1)) {
    queue_.record_buffer_error();
    return result;
  }
  if (data == nullptr) {
    queue_.record_buffer_size_query();
    return result;
  }

  auto* current = data;
  for (UINT i = 0; i < result && current != nullptr; ++i) {
    if (current->header.dwType == RIM_TYPEMOUSE) {
      if (const auto mutator = mouse_mutator_.load(std::memory_order_acquire); mutator != nullptr) {
        mutator(current->data.mouse);
      }
      queue_.record_buffered_observed();
      observe_mouse("buffer", reinterpret_cast<HRAWINPUT>(current), RID_INPUT, *current, sizeof(RAWINPUT), 0);
    } else {
      queue_.record_non_mouse();
    }
    if (current->header.dwSize == 0) {
      break;
    }
    current = reinterpret_cast<PRAWINPUT>(reinterpret_cast<std::byte*>(current) + current->header.dwSize);
  }
  return result;
}

BOOL RawInputObserver::observe_register_devices(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
  const auto fn = original_register_devices();
  if (fn == nullptr) {
    return FALSE;
  }

  const BOOL result = fn(devices, count, size);
  queue_.record_registration();

  for (UINT i = 0; i < count; ++i) {
    std::ostringstream row;
    const auto& device = devices[i];
    row << sequence_.fetch_add(1, std::memory_order_relaxed) + 1 << ',' << qpc_now()
        << ",register," << i << ',' << reinterpret_cast<std::uintptr_t>(device.hwndTarget) << ",0," << result << ','
        << count << ',' << size << ',' << device.usUsagePage << ',' << device.usUsage << ',' << device.dwFlags << ",0,0";
    csv_.write_row(row.str());
  }
  return result;
}

void RawInputObserver::observe_get_proc_address_request(const char* function_name, bool replaced) {
  if (!csv_.enabled()) {
    (void)function_name;
    return;
  }
  std::ostringstream row;
  row << sequence_.fetch_add(1, std::memory_order_relaxed) + 1 << ',' << qpc_now() << ",getproc,0,0,0,"
      << (replaced ? 1 : 0) << ",0,0,0,0,0,0,0";
  csv_.write_row(row.str());
  (void)function_name;
}

void RawInputObserver::observe_message(const char* source, const MSG& message) {
  if (message.message != WM_INPUT) {
    return;
  }
  queue_.record_wm_input_message();
  if (!csv_.enabled()) {
    return;
  }
  std::ostringstream row;
  row << sequence_.fetch_add(1, std::memory_order_relaxed) + 1 << ',' << qpc_now() << ',' << source << ",0,"
      << message.lParam << ',' << message.hwnd << ",1," << message.message << ',' << message.wParam << ",0,0,0,0,0";
  csv_.write_row(row.str());
}

void RawInputObserver::observe_cursor_point(
    const char* source, BOOL result, LONG x, LONG y, std::uintptr_t caller_rva, std::uintptr_t parent_rva) {
  if (!csv_.enabled()) {
    return;
  }
  std::ostringstream row;
  row << sequence_.fetch_add(1, std::memory_order_relaxed) + 1 << ',' << qpc_now() << ',' << source << ','
      << caller_rva << ",0,0," << result << ',' << parent_rva << ",0," << x << ',' << y << ",0,0,0";
  csv_.write_row(row.str());
}

void RawInputObserver::observe_cursor_rect(
    const char* source, BOOL result, const RECT* rect, std::uintptr_t caller_rva, std::uintptr_t parent_rva) {
  if (!csv_.enabled()) {
    return;
  }
  std::ostringstream row;
  row << sequence_.fetch_add(1, std::memory_order_relaxed) + 1 << ',' << qpc_now() << ',' << source << ','
      << caller_rva << ",0,0," << result << ',' << parent_rva << ",0,";
  if (rect != nullptr) {
    row << rect->left << ',' << rect->top << ',' << rect->right << ',' << rect->bottom;
  } else {
    row << "0,0,0,0";
  }
  row << ",0";
  csv_.write_row(row.str());
}

RawInputStatistics RawInputObserver::statistics() const {
  return queue_.statistics();
}

void RawInputObserver::observe_mouse(
    const char* source, HRAWINPUT input, UINT command, RAWINPUT& raw, UINT result, std::uintptr_t caller_rva) {
  const auto qpc = qpc_now();
  const bool duplicate = dedup_.is_duplicate(input, raw.data.mouse, qpc);

  RawInputEvent event;
  event.sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  event.qpc = qpc;
  event.handle = input;
  event.device = raw.header.hDevice;
  event.command = command;
  event.flags = raw.data.mouse.usFlags;
  event.x = raw.data.mouse.lLastX;
  event.y = raw.data.mouse.lLastY;
  event.button_flags = raw.data.mouse.usButtonFlags;
  event.button_data = raw.data.mouse.usButtonData;
  event.deduplicated = duplicate ? 1 : 0;
  queue_.push(event);
  if (!csv_.enabled()) {
    return;
  }

  std::ostringstream row;
  row << event.sequence << ',' << event.qpc << ',' << source << ',' << caller_rva << ','
      << reinterpret_cast<std::uintptr_t>(event.handle) << ',' << reinterpret_cast<std::uintptr_t>(event.device) << ',' << result
      << ',' << event.command << ',' << event.flags << ',' << event.x << ',' << event.y << ',' << event.button_flags << ','
      << event.button_data << ',' << event.deduplicated;
  csv_.write_row(row.str());
}

}  // namespace sade
