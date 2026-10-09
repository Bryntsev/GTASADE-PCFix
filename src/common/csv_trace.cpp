#include "common/csv_trace.h"

#include <Windows.h>

namespace sade {
namespace {

// Safety valve if the flusher thread is not running: never let a trace grow without bound.
constexpr std::size_t kMaxBufferedBytes = 32U * 1024U * 1024U;

}  // namespace

bool CsvTrace::open(const std::wstring& path, const std::string& header) {
  std::scoped_lock lock(file_mutex_, mutex_);
  file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    file_ = nullptr;
    return false;
  }
  buffer_.clear();
  buffer_.reserve(1024 * 1024);
  buffer_ += header;
  buffer_ += "\r\n";
  enabled_.store(true, std::memory_order_release);
  return true;
}

void CsvTrace::write_row(const std::string& row) {
  write_row(row.data(), row.size());
}

void CsvTrace::write_row(const char* row, std::size_t length) {
  if (!enabled()) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (buffer_.size() + length + 2 > kMaxBufferedBytes) {
    return;  // Drop rather than block the caller.
  }
  buffer_.append(row, length);
  buffer_ += "\r\n";
}

void CsvTrace::flush() {
  std::lock_guard file_lock(file_mutex_);
  if (file_ == nullptr) {
    return;
  }
  std::string pending;
  {
    std::lock_guard lock(mutex_);
    pending.swap(buffer_);
    buffer_.reserve(pending.capacity());
  }
  if (!pending.empty()) {
    DWORD written = 0;
    WriteFile(file_, pending.data(), static_cast<DWORD>(pending.size()), &written, nullptr);
  }
}

void CsvTrace::close() {
  enabled_.store(false, std::memory_order_release);
  flush();
  std::lock_guard file_lock(file_mutex_);
  if (file_ != nullptr) {
    CloseHandle(file_);
  }
  file_ = nullptr;
  std::lock_guard lock(mutex_);
  buffer_.clear();
}

}  // namespace sade
