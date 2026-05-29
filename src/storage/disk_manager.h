#ifndef BUFFER_MANAGER_SRC_STORAGE_DISK_MANAGER_H_
#define BUFFER_MANAGER_SRC_STORAGE_DISK_MANAGER_H_

#include <cstdint>
#include <filesystem>
#include <mutex>

#include "buffer_manager/page.h"
#include "buffer_manager/types.h"

namespace buffer_manager {

enum class IoMode : std::uint8_t {
  kDirect,
  kBuffered,
};

struct DiskManagerOptions final {
  std::filesystem::path path;
  PageId max_page_count = 0;
  std::uint64_t header_size = kPageSize;
  bool truncate_existing = false;
  bool preallocate = false;
  IoMode io_mode = IoMode::kBuffered;
};

class DiskManager final {
 public:
  explicit DiskManager(DiskManagerOptions options);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  DiskManager(DiskManager&&) = delete;
  DiskManager& operator=(DiskManager&&) = delete;

  void ReadPage(PageId page_id, Page* page) const;
  void WritePage(PageId page_id, const Page& page);

  void Sync() const;

 private:
  void ValidateOptions() const;
  void ValidatePageId(PageId page_id) const;

  void OpenFile();
  void ConfigureDirectIo() const;
  void InitializeFile() const;
  void WriteFreshHeader() const;
  void ValidateExistingHeader() const;
  void PreallocateFile() const;

  [[nodiscard]] std::uint64_t OffsetForPage(PageId page_id) const;
  [[nodiscard]] std::uint64_t ExpectedFileSize() const;

  DiskManagerOptions options_;
  int fd_ = -1;

  mutable std::mutex file_mutex_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_STORAGE_DISK_MANAGER_H_
