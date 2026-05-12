#ifndef BUFFER_MANAGER_TYPES_H_
#define BUFFER_MANAGER_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace buffer_manager {

inline constexpr std::size_t kPageSize = 1ULL << 12;

using PageId = std::uint64_t;
using FrameId = std::uint32_t;

}  // namespace buffer_manager

#endif  // BUFFER_MANAGER_TYPES_H_
