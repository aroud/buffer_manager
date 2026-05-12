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

  // Releases the pin before destruction, safe to call repeatedly
  void Drop() noexcept;

 private:
  friend class BufferManager;

  PageGuard(BufferManager* manager, PageId page_id, Page* page) noexcept;

  BufferManager* manager_ = nullptr;
  PageId page_id_ = 0;
  Page* page_ = nullptr;
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

  // Permanently deletes a logical page, throws if it is pinned
  void DeletePage(PageId page_id);

  // Explicitly flushes all dirty resident pages, throws on I/O failure
  void FlushAllPages();

  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] PageId max_page_count() const noexcept;

 private:
  friend class PageGuard;

  void UnpinPage(PageId page_id) noexcept;
  void MarkDirty(PageId page_id);
  void FlushPage(PageId page_id);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_BUFFER_MANAGER_H_