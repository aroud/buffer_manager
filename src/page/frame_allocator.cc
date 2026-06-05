#include "page/frame_allocator.h"

#include <bit>
#include <limits>
#include <stdexcept>

namespace buffer_manager {
namespace {

constexpr std::uint64_t kFullWord = ~std::uint64_t{0};

}  // namespace

FrameAllocator::FrameAllocator(std::size_t frame_count)
    : frame_count_(frame_count),
      frames_(frame_count),
      allocation_bitmap_((frame_count + kBitsPerWord - 1) / kBitsPerWord, 0) {
  if (frame_count == 0) {
    throw std::invalid_argument("frame_count must be greater than zero");
  }

  if (frame_count > std::numeric_limits<FrameId>::max()) {
    throw std::invalid_argument("frame_count does not fit into FrameId");
  }

  MaskUnusedTailBits();
}

std::optional<FrameId> FrameAllocator::AllocateFrame() {
  if (allocated_count_ == frame_count_) {
    return std::nullopt;
  }

  const std::size_t word_count = allocation_bitmap_.size();

  for (std::size_t probe = 0; probe < word_count; ++probe) {
    const std::size_t word_index = (next_search_word_ + probe) % word_count;
    const std::uint64_t used_word = allocation_bitmap_[word_index];

    if (used_word == kFullWord) {
      continue;
    }

    const std::uint64_t free_bits = ~used_word;
    const unsigned bit_index = std::countr_zero(free_bits);

    const auto frame_id =
        static_cast<FrameId>((word_index * kBitsPerWord) + bit_index);

    allocation_bitmap_[word_index] |= std::uint64_t{1} << bit_index;
    ++allocated_count_;

    if (allocation_bitmap_[word_index] == kFullWord) {
      next_search_word_ = (word_index + 1) % word_count;
    } else {
      next_search_word_ = word_index;
    }

    return frame_id;
  }

  return std::nullopt;
}

void FrameAllocator::FreeFrame(FrameId frame_id) {
  ValidateAllocatedFrameId(frame_id);

  const std::size_t word_index = WordIndex(frame_id);
  allocation_bitmap_[word_index] &= ~BitMask(frame_id);
  --allocated_count_;

  next_search_word_ = word_index;
}

Page& FrameAllocator::page(FrameId frame_id) {
  ValidateAllocatedFrameId(frame_id);
  return frames_[static_cast<std::size_t>(frame_id)];
}

const Page& FrameAllocator::page(FrameId frame_id) const {
  ValidateAllocatedFrameId(frame_id);
  return frames_[static_cast<std::size_t>(frame_id)];
}

bool FrameAllocator::IsAllocated(FrameId frame_id) const {
  ValidateFrameId(frame_id);
  return (allocation_bitmap_[WordIndex(frame_id)] & BitMask(frame_id)) != 0;
}

std::size_t FrameAllocator::frame_count() const noexcept {
  return frame_count_;
}

std::size_t FrameAllocator::allocated_count() const noexcept {
  return allocated_count_;
}

std::size_t FrameAllocator::free_count() const noexcept {
  return frame_count_ - allocated_count_;
}

std::size_t FrameAllocator::WordIndex(FrameId frame_id) {
  return static_cast<std::size_t>(frame_id) / kBitsPerWord;
}

std::uint64_t FrameAllocator::BitMask(FrameId frame_id) {
  return std::uint64_t{1} << (static_cast<std::size_t>(frame_id) %
                              kBitsPerWord);
}

void FrameAllocator::ValidateFrameId(FrameId frame_id) const {
  if (static_cast<std::size_t>(frame_id) >= frame_count_) {
    throw std::out_of_range("invalid frame id");
  }
}

void FrameAllocator::ValidateAllocatedFrameId(FrameId frame_id) const {
  ValidateFrameId(frame_id);

  if (!IsAllocated(frame_id)) {
    throw std::logic_error("frame is not allocated");
  }
}

void FrameAllocator::MaskUnusedTailBits() {
  const std::size_t valid_tail_bits = frame_count_ % kBitsPerWord;

  if (valid_tail_bits == 0) {
    return;
  }

  const std::uint64_t valid_mask =
      (std::uint64_t{1} << valid_tail_bits) - std::uint64_t{1};
  const std::uint64_t invalid_mask = ~valid_mask;

  allocation_bitmap_.back() |= invalid_mask;
}

}  // namespace buffer_manager