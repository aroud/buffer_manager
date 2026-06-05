#ifndef BUFFER_MANAGER_BENCHMARKS_BUFFER_MANAGER_ADAPTER_H_
#define BUFFER_MANAGER_BENCHMARKS_BUFFER_MANAGER_ADAPTER_H_

#include <cstddef>
#include <filesystem>
#include <future>
#include <optional>
#include <utility>

#include "buffer_manager/buffer_manager.h"

namespace buffer_manager::main_benchmark {

class BufferManagerAdapter final {
 public:
  struct PinnedPage final {
    PageId pfn = 0;
    PageGuard guard;

    PinnedPage(PageId page_id, PageGuard page_guard)
        : pfn(page_id), guard(std::move(page_guard)) {}

    PinnedPage(PinnedPage&&) noexcept = default;
    PinnedPage& operator=(PinnedPage&&) noexcept = default;

    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    [[nodiscard]] Page& page() {
      return guard.page();
    }

    [[nodiscard]] const Page& page() const {
      return guard.page();
    }

    void Drop() {
      guard.Drop();
    }
  };

  struct AsyncRequest final {
    PageId pfn = 0;
    std::future<PageGuard> future;
  };

  BufferManagerAdapter(const std::filesystem::path& path,
                       std::size_t frame_count, PageId max_page_count,
                       std::size_t async_worker_count,
                       IoMode io_mode = IoMode::kBuffered)
      : buffer_manager_({
            .disk =
                {
                    .path = path,
                    .max_page_count = max_page_count,
                    .truncate_existing = true,
                    .preallocate = true,
                    .io_mode = io_mode,
                },
            .frame_count = frame_count,
            .async_worker_count = async_worker_count,
        }) {}

  [[nodiscard]] PinnedPage AllocPageFrame(std::size_t thread_id) {
    (void)thread_id;

    PageGuard guard = buffer_manager_.NewPage();
    const PageId pfn = guard.page_id();

    return PinnedPage(pfn, std::move(guard));
  }

  [[nodiscard]] PinnedPage PFNToPage(PageId pfn, std::size_t thread_id) {
    (void)thread_id;

    return PinnedPage(pfn, buffer_manager_.FetchPage(pfn));
  }

  [[nodiscard]] AsyncRequest PFNToPageAsync(PageId pfn, std::size_t thread_id) {
    (void)thread_id;

    return AsyncRequest{
        .pfn = pfn,
        .future = buffer_manager_.FetchPageAsync(pfn),
    };
  }

  [[nodiscard]] static std::optional<PinnedPage> TryComplete(
      AsyncRequest& request) {
    using namespace std::chrono_literals;

    if (request.future.wait_for(0ns) != std::future_status::ready) {
      return std::nullopt;
    }

    return PinnedPage(request.pfn, request.future.get());
  }

  void static DecrementPinCount(PinnedPage& pinned_page) {
    pinned_page.Drop();
  }

  void FlushAllPages() {
    buffer_manager_.FlushAllPages();
  }

  void Sync() {
    buffer_manager_.Sync();
  }

 private:
  BufferManager buffer_manager_;
};

}  // namespace buffer_manager::main_benchmark

#endif  // BUFFER_MANAGER_BENCHMARKS_BUFFER_MANAGER_ADAPTER_H_