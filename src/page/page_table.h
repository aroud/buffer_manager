#ifndef BUFFER_MANAGER_SRC_PAGE_PAGE_TABLE_H_
#define BUFFER_MANAGER_SRC_PAGE_PAGE_TABLE_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "buffer_manager/types.h"

namespace buffer_manager {

enum class PageState : std::uint8_t {
  kUnused,
  kResident,
  kNonResident,
};

struct PageMeta final {
  PageState state = PageState::kUnused;
  std::optional<FrameId> frame_id;
  std::uint64_t disk_offset = 0;
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

  [[nodiscard]] PageMeta& Get(PageId page_id);
  [[nodiscard]] const PageMeta& Get(PageId page_id) const;

  [[nodiscard]] bool IsAllocated(PageId page_id) const;
  [[nodiscard]] PageId max_page_count() const noexcept;

 private:
  void ValidatePageId(PageId page_id) const;

  PageId max_page_count_ = 0;
  PageId next_page_id_ = 0;

  std::vector<PageMeta> pages_;
  std::vector<PageId> free_page_ids_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_PAGE_PAGE_TABLE_H_