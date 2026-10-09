#include "common/logger.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace sade {
namespace {

std::string timestamp_utc() {
  SYSTEMTIME st{};
  GetSystemTime(&st);
  char buffer[64]{};
  std::snprintf(buffer,
                sizeof(buffer),
                "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                st.wYear,
                st.wMonth,
                st.wDay,
                st.wHour,
                st.wMinute,
                st.wSecond,
                st.wMilliseconds);
  return buffer;
}

}  // namespace

bool Logger::open(const std::wstring& path) {
  std::lock_guard lock(mutex_);
  file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  buffer_.clear();
  return file_ != INVALID_HANDLE_VALUE;
}

void Logger::info(const std::string& message) {
  write_line("INFO", message);
}

void Logger::warn(const std::string& message) {
  write_line("WARN", message);
}

void Logger::error(const std::string& message) {
  write_line("ERROR", message);
}

void Logger::close() {
  std::lock_guard lock(mutex_);
  if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) {
    flush_locked();
    CloseHandle(file_);
  }
  file_ = nullptr;
  buffer_.clear();
}

void Logger::write_line(const char* level, const std::string& message) {
  std::lock_guard lock(mutex_);
  if (file_ == nullptr || file_ == INVALID_HANDLE_VALUE) {
    return;
  }
  const std::string line = timestamp_utc() + " [" + level + "] " + message + "\r\n";
  buffer_ += line;
  // The log is low volume; flushing every line keeps it useful when the game crashes.
  flush_locked();
}

void Logger::flush_locked() {
  if (file_ == nullptr || file_ == INVALID_HANDLE_VALUE || buffer_.empty()) {
    return;
  }
  DWORD written = 0;
  WriteFile(file_, buffer_.data(), static_cast<DWORD>(buffer_.size()), &written, nullptr);
  buffer_.clear();
}

}  // namespace sade
