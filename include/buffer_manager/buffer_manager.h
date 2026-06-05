#ifndef BUFFER_MANAGER_BUFFER_MANAGER_H_
#define BUFFER_MANAGER_BUFFER_MANAGER_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "buffer_manager/page.h"
#include "buffer_manager/types.h"
#include "page/frame_allocator.h"
#include "page/page_table.h"
#include "replacement/replacer.h"
#include "storage/disk_manager.h"

namespace buffer_manager {

struct BufferManagerOptions final {
  DiskManagerOptions disk;
  std::size_t frame_count = 0;
  std::size_t async_worker_count = 2;
};

class BufferManager;

class PageGuard final {
 public:
  PageGuard() = default;
  ~PageGuard() noexcept;

  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] PageId page_id() const;
  [[nodiscard]] Page& page();
  [[nodiscard]] const Page& page() const;

  void MarkDirty();
  void Drop();

 private:
  friend class BufferManager;

  PageGuard(BufferManager* buffer_manager, PageId page_id, FrameId frame_id,
            Page* page) noexcept;

  BufferManager* buffer_manager_ = nullptr;
  PageId page_id_ = 0;
  FrameId frame_id_ = 0;
  Page* page_ = nullptr;
};

class BufferManager final {
 public:
  explicit BufferManager(BufferManagerOptions options,
                         std::unique_ptr<Replacer> replacer = nullptr);
  ~BufferManager();

  BufferManager(const BufferManager&) = delete;
  BufferManager& operator=(const BufferManager&) = delete;

  BufferManager(BufferManager&&) = delete;
  BufferManager& operator=(BufferManager&&) = delete;

  [[nodiscard]] PageGuard NewPage();
  [[nodiscard]] PageGuard FetchPage(PageId page_id);

  [[nodiscard]] std::future<PageGuard> NewPageAsync();
  [[nodiscard]] std::future<PageGuard> FetchPageAsync(PageId page_id);

  void FlushPage(PageId page_id);
  void FlushAllPages();
  void Sync() const;

  [[nodiscard]] std::future<void> FlushPageAsync(PageId page_id);
  [[nodiscard]] std::future<void> FlushAllPagesAsync();
  [[nodiscard]] std::future<void> SyncAsync();

  void DeletePage(PageId page_id);
  [[nodiscard]] std::future<void> DeletePageAsync(PageId page_id);

  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] PageId max_page_count() const noexcept;

 private:
  friend class PageGuard;

  class AsyncExecutor;

  enum class FrameState : std::uint8_t {
    kFree,
    kLoading,
    kResident,
    kEvicting,
  };

  struct FrameMeta final {
    std::optional<PageId> page_id;
    std::uint32_t pin_count = 0;
    bool dirty = false;
    std::uint64_t dirty_epoch = 0;
    FrameState state = FrameState::kFree;
  };

  [[nodiscard]] FrameId AcquireFrameLocked(std::unique_lock<std::mutex>& lock);
  [[nodiscard]] std::optional<FrameId> AcquireFrameForFetchLocked(
      PageId page_id, std::unique_lock<std::mutex>& lock);
  [[nodiscard]] bool HasTransientFrameLocked() const noexcept;

  void RestoreEvictedFrameLocked(FrameId frame_id, PageId page_id, bool dirty,
                                 std::uint64_t dirty_epoch);

  [[nodiscard]] PageGuard MakeGuardLocked(PageId page_id, FrameId frame_id);
  void PinResidentFrameLocked(PageId page_id, FrameId frame_id);
  void UnpinFrameLocked(FrameId frame_id);

  void DropPageGuard(PageId page_id, FrameId frame_id);
  void MarkDirty(PageId page_id, FrameId frame_id);

  void ValidateOptions() const;

  BufferManagerOptions options_;

  DiskManager disk_manager_;
  FrameAllocator frame_allocator_;
  PageTable page_table_;
  std::unique_ptr<Replacer> replacer_;
  std::vector<FrameMeta> frame_meta_;

  mutable std::mutex mutex_;
  std::condition_variable state_changed_;

  std::unique_ptr<AsyncExecutor> async_executor_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_BUFFER_MANAGER_H_