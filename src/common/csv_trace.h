#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace sade {

// Buffered CSV writer. write_row only appends to memory, so it is safe on game threads;
// the runtime's trace flusher thread calls flush() to move data to disk.
class CsvTrace {
 public:
  bool open(const std::wstring& path, const std::string& header);
  // Cheap check so callers can skip formatting rows nobody will write.
  bool enabled() const { return enabled_.load(std::memory_order_acquire); }
  void write_row(const std::string& row);
  void write_row(const char* row, std::size_t length);
  void flush();
  void close();

 private:
  std::mutex mutex_;
  std::mutex file_mutex_;
  std::atomic<bool> enabled_{false};
  void* file_ = nullptr;
  std::string buffer_;
};

}  // namespace sade
