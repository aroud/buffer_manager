#ifndef BUFFER_MANAGER_SRC_PAGE_PAGE_TABLE_H_
#define BUFFER_MANAGER_SRC_PAGE_PAGE_TABLE_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "buffer_manager/types.h"

namespace buffer_manager {

enum class PageState : std::uint8_t {
  kUnused,
  kNonResident,
  kLoading,
  kResident,
  kEvicting,
};

class PageTable final {
 public:
  explicit PageTable(PageId max_page_count);

  PageTable(const PageTable&) = delete;
  PageTable& operator=(const PageTable&) = delete;

  PageTable(PageTable&&) = delete;
  PageTable& operator=(PageTable&&) = delete;

  [[nodiscard]] PageId AllocatePageId();
  void FreePageId(PageId page_id);

  [[nodiscard]] bool IsAllocated(PageId page_id) const;
  [[nodiscard]] bool IsResident(PageId page_id) const;
  [[nodiscard]] PageState State(PageId page_id) const;
  [[nodiscard]] std::optional<FrameId> FrameForPage(PageId page_id) const;

  void SetLoading(PageId page_id, FrameId frame_id);
  void SetResident(PageId page_id, FrameId frame_id);
  void SetNonResident(PageId page_id);
  void SetEvicting(PageId page_id);

  [[nodiscard]] PageId max_page_count() const noexcept;
  [[nodiscard]] PageId allocated_count() const noexcept;

 private:
  struct Entry final {
    PageState state = PageState::kUnused;
    std::optional<FrameId> frame_id;
  };

  void ValidatePageId(PageId page_id) const;
  [[nodiscard]] Entry& EntryFor(PageId page_id);
  [[nodiscard]] const Entry& EntryFor(PageId page_id) const;

  PageId max_page_count_ = 0;
  PageId next_page_id_ = 0;
  PageId allocated_count_ = 0;

  std::vector<Entry> pages_;
  std::vector<PageId> free_page_ids_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_PAGE_PAGE_TABLE_H_