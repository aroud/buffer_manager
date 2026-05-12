#ifndef BUFFER_MANAGER_BUFFER_MANAGER_H_
#define BUFFER_MANAGER_BUFFER_MANAGER_H_

#include <cstddef>
#include <filesystem>
#include <memory>

#include "buffer_manager/page.h"
#include "buffer_manager/types.h"

namespace buffer_manager {

class BufferManager;

struct BufferManagerOptions final {
  std::size_t frame_count = 0;
  PageId max_page_count = 0;
  std::filesystem::path backing_file;
};

class PageGuard final {
 public:
  PageGuard() noexcept;
  ~PageGuard() noexcept;

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;

  [[nodiscard]] bool is_valid() const noexcept;

  [[nodiscard]] PageId page_id() const;
  [[nodiscard]] Page& page();
  [[nodiscard]] const Page& page() const;

  void MarkDirty();
  void Flush();

  // Releases the pin before destruction, safe to call multiple times
  void Drop() noexcept;

 private:
  friend class BufferManager;

  PageGuard(BufferManager* manager, PageId page_id, Page* page) noexcept;

  BufferManager* manager_ = nullptr;
  Page* page_ = nullptr;
  PageId page_id_ = 0;
};

class BufferManager final {
 public:
  explicit BufferManager(BufferManagerOptions options);
  ~BufferManager();

  BufferManager(const BufferManager&) = delete;
  BufferManager& operator=(const BufferManager&) = delete;

  BufferManager(BufferManager&&) = delete;
  BufferManager& operator=(BufferManager&&) = delete;

  // Allocates a new logical page and returns it pinned
  [[nodiscard]] PageGuard AllocatePage();

  // Loads an existing logical page and returns it pinned
  [[nodiscard]] PageGuard FetchPage(PageId page_id);

  // Permanently deletes a logical page, throws if the page is currently pinned
  void DeletePage(PageId page_id);

  // Explicitly flushes all dirty resident pages, may throw on I/O failure
  void FlushAllPages();

  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] PageId max_page_count() const noexcept;

 private:
  friend class PageGuard;

  // Called only by PageGuard, including PageGuard::~PageGuard()
  void UnpinPage(PageId page_id) noexcept;

  // Called only by PageGuard
  void MarkDirty(PageId page_id);

  // Called only by PageGuard, may throw on I/O failure
  void FlushPage(PageId page_id);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_BUFFER_MANAGER_H_