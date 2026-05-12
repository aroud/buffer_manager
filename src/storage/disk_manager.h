#ifndef BUFFER_MANAGER_SRC_STORAGE_DISK_MANAGER_H_
#define BUFFER_MANAGER_SRC_STORAGE_DISK_MANAGER_H_

#include <filesystem>
#include <fstream>

#include "buffer_manager/page.h"

namespace buffer_manager {

class DiskManager final {
 public:
  explicit DiskManager(const std::filesystem::path& path);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  DiskManager(DiskManager&&) = delete;
  DiskManager& operator=(DiskManager&&) = delete;

  void ReadPage(std::uint64_t disk_offset, Page* page);
  void WritePage(std::uint64_t disk_offset, const Page& page);

 private:
  std::fstream file_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_STORAGE_DISK_MANAGER_H_