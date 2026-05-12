#ifndef BUFFER_MANAGER_PAGE_H_
#define BUFFER_MANAGER_PAGE_H_

#include <array>
#include <cstddef>

#include "buffer_manager/types.h"

namespace buffer_manager {

struct alignas(kPageSize) Page final {
  std::array<std::byte, kPageSize> data{};
};

static_assert(sizeof(Page) == kPageSize);
static_assert(alignof(Page) == kPageSize);

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_PAGE_H_
