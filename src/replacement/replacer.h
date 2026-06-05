#ifndef BUFFER_MANAGER_SRC_REPLACEMENT_REPLACER_H_
#define BUFFER_MANAGER_SRC_REPLACEMENT_REPLACER_H_

#include <cstddef>
#include <optional>

#include "buffer_manager/types.h"

namespace buffer_manager {

class Replacer {
 public:
  virtual ~Replacer();

  Replacer(const Replacer&) = delete;
  Replacer& operator=(const Replacer&) = delete;

  Replacer(Replacer&&) = delete;
  Replacer& operator=(Replacer&&) = delete;

  virtual void RecordAccess(FrameId frame_id) = 0;
  virtual void SetEvictable(FrameId frame_id, bool evictable) = 0;
  [[nodiscard]] virtual std::optional<FrameId> Victim() = 0;
  virtual void Remove(FrameId frame_id) = 0;

  [[nodiscard]] virtual std::size_t evictable_count() const noexcept = 0;

 protected:
  Replacer() = default;
};

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_SRC_REPLACEMENT_REPLACER_H_