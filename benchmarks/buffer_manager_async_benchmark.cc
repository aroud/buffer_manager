#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "buffer_manager/buffer_manager.h"
#include "replacement/clock_replacer.h"

namespace buffer_manager {

class BufferManager::AsyncExecutor final {
 public:
  explicit AsyncExecutor(std::size_t worker_count) {
    if (worker_count == 0) {
      throw std::invalid_argument(
          "async_worker_count must be greater than zero");
    }

    workers_.reserve(worker_count);

    for (std::size_t i = 0; i < worker_count; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  ~AsyncExecutor() {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }

    ready_.notify_all();

    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  AsyncExecutor(const AsyncExecutor&) = delete;
  AsyncExecutor& operator=(const AsyncExecutor&) = delete;

  AsyncExecutor(AsyncExecutor&&) = delete;
  AsyncExecutor& operator=(AsyncExecutor&&) = delete;

  template <typename Function>
  [[nodiscard]] auto Submit(Function&& function)
      -> std::future<std::invoke_result_t<std::decay_t<Function>&>> {
    using FunctionType = std::decay_t<Function>;
    using Result = std::invoke_result_t<FunctionType&>;

    auto task = std::make_shared<std::packaged_task<Result()>>(
        std::forward<Function>(function));

    std::future<Result> future = task->get_future();

    {
      std::scoped_lock lock(mutex_);

      if (stopping_) {
        throw std::logic_error("async executor is stopping");
      }

      tasks_.emplace_back([task] { (*task)(); });
    }

    ready_.notify_one();
    return future;
  }

 private:
  void WorkerLoop() {
    for (;;) {
      std::function<void()> task;

      {
        std::unique_lock lock(mutex_);

        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

        if (stopping_ && tasks_.empty()) {
          return;
        }

        task = std::move(tasks_.front());
        tasks_.pop_front();
      }

      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  bool stopping_ = false;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
};

PageGuard::PageGuard(BufferManager* buffer_manager, PageId page_id,
                     FrameId frame_id, Page* page) noexcept
    : buffer_manager_(buffer_manager),
      page_id_(page_id),
      frame_id_(frame_id),
      page_(page) {}

PageGuard::~PageGuard() noexcept {
  if (!valid()) {
    return;
  }

  try {
    Drop();
  } catch (...) {
    std::terminate();
  }
}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : buffer_manager_(std::exchange(other.buffer_manager_, nullptr)),
      page_id_(std::exchange(other.page_id_, 0)),
      frame_id_(std::exchange(other.frame_id_, 0)),
      page_(std::exchange(other.page_, nullptr)) {}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  try {
    Drop();
  } catch (...) {
    std::terminate();
  }

  buffer_manager_ = std::exchange(other.buffer_manager_, nullptr);
  page_id_ = std::exchange(other.page_id_, 0);
  frame_id_ = std::exchange(other.frame_id_, 0);
  page_ = std::exchange(other.page_, nullptr);

  return *this;
}

bool PageGuard::valid() const noexcept {
  return buffer_manager_ != nullptr;
}

PageId PageGuard::page_id() const {
  if (!valid()) {
    throw std::logic_error("page guard is empty");
  }

  return page_id_;
}

Page& PageGuard::page() {
  if (!valid()) {
    throw std::logic_error("page guard is empty");
  }

  return *page_;
}

const Page& PageGuard::page() const {
  if (!valid()) {
    throw std::logic_error("page guard is empty");
  }

  return *page_;
}

void PageGuard::MarkDirty() {
  if (!valid()) {
    throw std::logic_error("page guard is empty");
  }

  buffer_manager_->MarkDirty(page_id_, frame_id_);
}

void PageGuard::Drop() {
  if (!valid()) {
    return;
  }

  BufferManager* buffer_manager = buffer_manager_;
  const PageId page_id = page_id_;
  const FrameId frame_id = frame_id_;

  buffer_manager_ = nullptr;
  page_id_ = 0;
  frame_id_ = 0;
  page_ = nullptr;

  buffer_manager->DropPageGuard(page_id, frame_id);
}

BufferManager::BufferManager(BufferManagerOptions options,
                             std::unique_ptr<Replacer> replacer)
    : options_(std::move(options)),
      disk_manager_(options_.disk),
      frame_allocator_(options_.frame_count),
      page_table_(options_.disk.max_page_count),
      replacer_(std::move(replacer)),
      frame_meta_(options_.frame_count),
      async_executor_(
          std::make_unique<AsyncExecutor>(options_.async_worker_count)) {
  ValidateOptions();

  if (replacer_ == nullptr) {
    replacer_ = std::make_unique<ClockReplacer>(options_.frame_count);
  }
}

BufferManager::~BufferManager() = default;

PageGuard BufferManager::NewPage() {
  std::unique_lock lock(mutex_);

  const PageId page_id = page_table_.AllocatePageId();

  try {
    const FrameId frame_id = AcquireFrameLocked(lock);

    Page& page = frame_allocator_.page(frame_id);
    page.data.fill(std::byte{0});

    frame_meta_[static_cast<std::size_t>(frame_id)] = FrameMeta{
        .page_id = page_id,
        .pin_count = 1,
        .dirty = true,
        .dirty_epoch = 1,
        .state = FrameState::kResident,
    };

    page_table_.SetResident(page_id, frame_id);

    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);

    state_changed_.notify_all();
    return MakeGuardLocked(page_id, frame_id);
  } catch (...) {
    if (page_table_.State(page_id) == PageState::kNonResident) {
      page_table_.FreePageId(page_id);
    }

    state_changed_.notify_all();
    throw;
  }
}

PageGuard BufferManager::FetchPage(PageId page_id) {
  std::unique_lock lock(mutex_);

  for (;;) {
    const PageState state = page_table_.State(page_id);

    switch (state) {
      case PageState::kUnused:
        throw std::logic_error("cannot fetch an unused page");

      case PageState::kResident: {
        const FrameId frame_id = *page_table_.FrameForPage(page_id);
        PinResidentFrameLocked(page_id, frame_id);
        return MakeGuardLocked(page_id, frame_id);
      }

      case PageState::kLoading:
      case PageState::kEvicting:
        state_changed_.wait(lock);
        break;

      case PageState::kNonResident: {
        const std::optional<FrameId> acquired_frame =
            AcquireFrameForFetchLocked(page_id, lock);

        if (!acquired_frame.has_value()) {
          break;
        }

        const FrameId frame_id = *acquired_frame;

        page_table_.SetLoading(page_id, frame_id);
        frame_meta_[static_cast<std::size_t>(frame_id)] = FrameMeta{
            .page_id = page_id,
            .pin_count = 1,
            .dirty = false,
            .dirty_epoch = 0,
            .state = FrameState::kLoading,
        };

        Page& frame = frame_allocator_.page(frame_id);

        lock.unlock();

        try {
          disk_manager_.ReadPage(page_id, &frame);
        } catch (...) {
          lock.lock();

          frame_meta_[static_cast<std::size_t>(frame_id)] = FrameMeta{};
          frame_allocator_.FreeFrame(frame_id);
          page_table_.SetNonResident(page_id);

          state_changed_.notify_all();
          throw;
        }

        lock.lock();

        FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];
        meta.state = FrameState::kResident;
        meta.dirty = false;
        meta.dirty_epoch = 0;

        page_table_.SetResident(page_id, frame_id);

        replacer_->RecordAccess(frame_id);
        replacer_->SetEvictable(frame_id, false);

        state_changed_.notify_all();
        return MakeGuardLocked(page_id, frame_id);
      }
    }
  }
}

std::future<PageGuard> BufferManager::NewPageAsync() {
  return async_executor_->Submit([this] { return NewPage(); });
}

std::future<PageGuard> BufferManager::FetchPageAsync(PageId page_id) {
  return async_executor_->Submit(
      [this, page_id] { return FetchPage(page_id); });
}

void BufferManager::FlushPage(PageId page_id) {
  std::unique_lock lock(mutex_);

  for (;;) {
    const PageState state = page_table_.State(page_id);

    switch (state) {
      case PageState::kUnused:
        throw std::logic_error("cannot flush an unused page");

      case PageState::kNonResident:
        return;

      case PageState::kLoading:
      case PageState::kEvicting:
        state_changed_.wait(lock);
        break;

      case PageState::kResident: {
        const FrameId frame_id = *page_table_.FrameForPage(page_id);
        FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

        if (!meta.dirty) {
          return;
        }

        if (meta.page_id != page_id || meta.state != FrameState::kResident) {
          throw std::logic_error("inconsistent resident page metadata");
        }

        ++meta.pin_count;
        replacer_->SetEvictable(frame_id, false);

        const std::uint64_t captured_dirty_epoch = meta.dirty_epoch;
        Page& page = frame_allocator_.page(frame_id);

        lock.unlock();

        try {
          disk_manager_.WritePage(page_id, page);
        } catch (...) {
          lock.lock();
          UnpinFrameLocked(frame_id);
          state_changed_.notify_all();
          throw;
        }

        lock.lock();

        FrameMeta& current_meta =
            frame_meta_[static_cast<std::size_t>(frame_id)];

        if (current_meta.page_id == page_id &&
            current_meta.state == FrameState::kResident &&
            current_meta.dirty_epoch == captured_dirty_epoch) {
          current_meta.dirty = false;
        }

        UnpinFrameLocked(frame_id);
        state_changed_.notify_all();
        return;
      }
    }
  }
}

std::future<void> BufferManager::FlushPageAsync(PageId page_id) {
  return async_executor_->Submit([this, page_id] { FlushPage(page_id); });
}

void BufferManager::FlushAllPages() {
  std::vector<PageId> dirty_pages;

  {
    std::scoped_lock lock(mutex_);

    dirty_pages.reserve(frame_meta_.size());

    for (const FrameMeta& meta : frame_meta_) {
      if (meta.state == FrameState::kResident && meta.dirty &&
          meta.page_id.has_value()) {
        dirty_pages.push_back(*meta.page_id);
      }
    }
  }

  for (PageId page_id : dirty_pages) {
    FlushPage(page_id);
  }
}

std::future<void> BufferManager::FlushAllPagesAsync() {
  return async_executor_->Submit([this] { FlushAllPages(); });
}

void BufferManager::Sync() const {
  disk_manager_.Sync();
}

std::future<void> BufferManager::SyncAsync() {
  return async_executor_->Submit([this] { Sync(); });
}

void BufferManager::DeletePage(PageId page_id) {
  std::unique_lock lock(mutex_);

  for (;;) {
    const PageState state = page_table_.State(page_id);

    switch (state) {
      case PageState::kUnused:
        return;

      case PageState::kLoading:
      case PageState::kEvicting:
        state_changed_.wait(lock);
        break;

      case PageState::kResident: {
        const FrameId frame_id = *page_table_.FrameForPage(page_id);
        FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

        if (meta.page_id != page_id || meta.state != FrameState::kResident) {
          throw std::logic_error("inconsistent resident page metadata");
        }

        if (meta.pin_count != 0) {
          throw std::logic_error("cannot delete a pinned page");
        }

        replacer_->Remove(frame_id);
        frame_meta_[static_cast<std::size_t>(frame_id)] = FrameMeta{};
        frame_allocator_.FreeFrame(frame_id);

        page_table_.SetNonResident(page_id);
        page_table_.FreePageId(page_id);

        state_changed_.notify_all();
        return;
      }

      case PageState::kNonResident:
        page_table_.FreePageId(page_id);
        state_changed_.notify_all();
        return;
    }
  }
}

std::future<void> BufferManager::DeletePageAsync(PageId page_id) {
  return async_executor_->Submit([this, page_id] { DeletePage(page_id); });
}

std::size_t BufferManager::frame_count() const noexcept {
  return options_.frame_count;
}

PageId BufferManager::max_page_count() const noexcept {
  return options_.disk.max_page_count;
}

FrameId BufferManager::AcquireFrameLocked(std::unique_lock<std::mutex>& lock) {
  if (!lock.owns_lock()) {
    throw std::logic_error("metadata lock must be held");
  }

  for (;;) {
    if (auto free_frame = frame_allocator_.AllocateFrame()) {
      return *free_frame;
    }

    std::optional<FrameId> victim = replacer_->Victim();

    if (!victim.has_value()) {
      if (HasTransientFrameLocked()) {
        state_changed_.wait(lock);
        continue;
      }

      throw std::runtime_error("no frame available");
    }

    const FrameId frame_id = *victim;
    FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

    if (!meta.page_id.has_value()) {
      throw std::logic_error("victim frame has no page");
    }

    if (meta.pin_count != 0) {
      throw std::logic_error("replacer selected a pinned frame");
    }

    if (meta.state != FrameState::kResident) {
      throw std::logic_error("victim frame is not resident");
    }

    const PageId old_page_id = *meta.page_id;
    const bool dirty = meta.dirty;
    const std::uint64_t dirty_epoch = meta.dirty_epoch;

    page_table_.SetEvicting(old_page_id);
    meta.state = FrameState::kEvicting;

    Page& old_page = frame_allocator_.page(frame_id);

    lock.unlock();

    try {
      if (dirty) {
        disk_manager_.WritePage(old_page_id, old_page);
      }
    } catch (...) {
      lock.lock();
      RestoreEvictedFrameLocked(frame_id, old_page_id, dirty, dirty_epoch);
      state_changed_.notify_all();
      throw;
    }

    lock.lock();

    page_table_.SetNonResident(old_page_id);
    frame_meta_[static_cast<std::size_t>(frame_id)] = FrameMeta{};

    state_changed_.notify_all();
    return frame_id;
  }
}

std::optional<FrameId> BufferManager::AcquireFrameForFetchLocked(
    PageId page_id, std::unique_lock<std::mutex>& lock) {
  if (!lock.owns_lock()) {
    throw std::logic_error("metadata lock must be held");
  }

  for (;;) {
    if (page_table_.State(page_id) != PageState::kNonResident) {
      return std::nullopt;
    }

    if (auto free_frame = frame_allocator_.AllocateFrame()) {
      return free_frame;
    }

    std::optional<FrameId> victim = replacer_->Victim();

    if (!victim.has_value()) {
      if (HasTransientFrameLocked()) {
        state_changed_.wait(lock);
        continue;
      }

      throw std::runtime_error("no frame available");
    }

    const FrameId frame_id = *victim;
    FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

    if (!meta.page_id.has_value()) {
      throw std::logic_error("victim frame has no page");
    }

    if (meta.pin_count != 0) {
      throw std::logic_error("replacer selected a pinned frame");
    }

    if (meta.state != FrameState::kResident) {
      throw std::logic_error("victim frame is not resident");
    }

    const PageId old_page_id = *meta.page_id;
    const bool dirty = meta.dirty;
    const std::uint64_t dirty_epoch = meta.dirty_epoch;

    page_table_.SetEvicting(old_page_id);
    meta.state = FrameState::kEvicting;

    Page& old_page = frame_allocator_.page(frame_id);

    lock.unlock();

    try {
      if (dirty) {
        disk_manager_.WritePage(old_page_id, old_page);
      }
    } catch (...) {
      lock.lock();
      RestoreEvictedFrameLocked(frame_id, old_page_id, dirty, dirty_epoch);
      state_changed_.notify_all();
      throw;
    }

    lock.lock();

    page_table_.SetNonResident(old_page_id);
    frame_meta_[static_cast<std::size_t>(frame_id)] = FrameMeta{};

    state_changed_.notify_all();
    return frame_id;
  }
}

bool BufferManager::HasTransientFrameLocked() const noexcept {
  return std::ranges::any_of(frame_meta_, [](const FrameMeta& meta) {
    return meta.state == FrameState::kLoading ||
           meta.state == FrameState::kEvicting;
  });
}

void BufferManager::RestoreEvictedFrameLocked(FrameId frame_id, PageId page_id,
                                              bool dirty,
                                              std::uint64_t dirty_epoch) {
  FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

  if (page_table_.State(page_id) == PageState::kEvicting) {
    page_table_.SetNonResident(page_id);
    page_table_.SetResident(page_id, frame_id);
  }

  meta = FrameMeta{
      .page_id = page_id,
      .pin_count = 0,
      .dirty = dirty,
      .dirty_epoch = dirty_epoch,
      .state = FrameState::kResident,
  };

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, true);
}

PageGuard BufferManager::MakeGuardLocked(PageId page_id, FrameId frame_id) {
  Page& page = frame_allocator_.page(frame_id);
  return PageGuard(this, page_id, frame_id, &page);
}

void BufferManager::PinResidentFrameLocked(PageId page_id, FrameId frame_id) {
  FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

  if (meta.page_id != page_id || meta.state != FrameState::kResident) {
    throw std::logic_error("inconsistent resident page metadata");
  }

  if (meta.pin_count == std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("pin count overflow");
  }

  ++meta.pin_count;

  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
}

void BufferManager::UnpinFrameLocked(FrameId frame_id) {
  FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

  if (meta.state != FrameState::kResident) {
    throw std::logic_error("cannot unpin a nonresident frame");
  }

  if (meta.pin_count == 0) {
    throw std::logic_error("pin count underflow");
  }

  --meta.pin_count;

  if (meta.pin_count == 0) {
    replacer_->SetEvictable(frame_id, true);
  }
}

void BufferManager::DropPageGuard(PageId page_id, FrameId frame_id) {
  std::scoped_lock lock(mutex_);

  FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

  if (meta.page_id != page_id) {
    throw std::logic_error("page guard refers to the wrong page");
  }

  UnpinFrameLocked(frame_id);
  state_changed_.notify_all();
}

void BufferManager::MarkDirty(PageId page_id, FrameId frame_id) {
  std::scoped_lock lock(mutex_);

  FrameMeta& meta = frame_meta_[static_cast<std::size_t>(frame_id)];

  if (meta.page_id != page_id || meta.state != FrameState::kResident) {
    throw std::logic_error("cannot mark a nonresident page dirty");
  }

  meta.dirty = true;
  ++meta.dirty_epoch;
}

void BufferManager::ValidateOptions() const {
  if (options_.frame_count == 0) {
    throw std::invalid_argument("frame_count must be greater than zero");
  }

  if (options_.disk.max_page_count == 0) {
    throw std::invalid_argument("max_page_count must be greater than zero");
  }

  if (options_.async_worker_count == 0) {
    throw std::invalid_argument("async_worker_count must be greater than zero");
  }
}

}  // namespace buffer_manager
