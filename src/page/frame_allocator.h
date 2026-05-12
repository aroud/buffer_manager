#ifndef BUFFER_MANAGER_SRC_PAGE_FRAME_ALLOCATOR_H_
#define BUFFER_MANAGER_SRC_PAGE_FRAME_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "buffer_manager/page.h"
#include "buffer_manager/types.h"

namespace buffer_manager {

class FrameAllocator final {
 public:
  explicit FrameAllocator(std::size_t frame_count);

  FrameAllocator(const FrameAllocator&) = delete;
  FrameAllocator& operator=(const FrameAllocator&) = delete;

  FrameAllocator(FrameAllocator&&) = delete;
  FrameAllocator& operator=(FrameAllocator&&) = delete;

  [[nodiscard]] std::optional<FrameId> AllocateFrame();
  void FreeFrame(FrameId frame_id);

  [[nodiscard]] Page& page(FrameId frame_id);
  [[nodiscard]] const Page& page(FrameId frame_id) const;

  [[nodiscard]] bool IsAllocated(FrameId frame_id) const;

  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] std::size_t used_count() const noexcept;

 private:
  static constexpr std::size_t kBitsPerWord = 64;

  [[nodiscard]] std::size_t WordIndex(FrameId frame_id) const;
  [[nodiscard]] std::uint64_t BitMask(FrameId frame_id) const;

  void ValidateFrameId(FrameId frame_id) const;
  void MaskUnusedTailBits();

  std::size_t frame_count_ = 0;
  std::size_t used_count_ = 0;
  std::size_t next_search_word_ = 0;

  std::vector<Page> frames_;
  std::vector<std::uint64_t> allocation_bitmap_;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_PAGE_FRAME_ALLOCATOR_H_