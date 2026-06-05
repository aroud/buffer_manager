#ifndef BUFFER_MANAGER_SRC_REPLACEMENT_CLOCK_REPLACER_H_
#define BUFFER_MANAGER_SRC_REPLACEMENT_CLOCK_REPLACER_H_

#include <cstddef>
#include <optional>
#include <vector>

#include "buffer_manager/types.h"
#include "replacement/replacer.h"

namespace buffer_manager {

class ClockReplacer final : public Replacer {
 public:
  explicit ClockReplacer(std::size_t frame_count);

  void RecordAccess(FrameId frame_id) override;
  void SetEvictable(FrameId frame_id, bool evictable) override;
  [[nodiscard]] std::optional<FrameId> Victim() override;
  void Remove(FrameId frame_id) override;

  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] std::size_t evictable_count() const noexcept override;

 private:
  struct Entry final {
    bool present = false;
    bool evictable = false;
    bool referenced = false;
  };

  void ValidateFrameId(FrameId frame_id) const;
  void AdvanceClockHand() noexcept;

  std::vector<Entry> entries_;
  std::size_t clock_hand_ = 0;
  std::size_t evictable_count_ = 0;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_REPLACEMENT_CLOCK_REPLACER_H_