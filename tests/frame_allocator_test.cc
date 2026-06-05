#include "page/frame_allocator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace buffer_manager {
namespace {

bool IsPageAligned(const Page& page) {
  const auto address = reinterpret_cast<std::uintptr_t>(page.data.data());
  return address % kPageSize == 0;
}

TEST(FrameAllocatorTest, ConstructorRejectsZeroFrames) {
  EXPECT_THROW(FrameAllocator(0), std::invalid_argument);
}

TEST(FrameAllocatorTest, StartsWithAllFramesFree) {
  FrameAllocator allocator(8);

  EXPECT_EQ(allocator.frame_count(), 8U);
  EXPECT_EQ(allocator.allocated_count(), 0U);
  EXPECT_EQ(allocator.free_count(), 8U);
}

TEST(FrameAllocatorTest, AllocatesFramesUntilFull) {
  FrameAllocator allocator(4);
  std::unordered_set<FrameId> allocated;

  for (std::size_t i = 0; i < 4; ++i) {
    std::optional<FrameId> frame_id = allocator.AllocateFrame();

    ASSERT_TRUE(frame_id.has_value());
    EXPECT_TRUE(allocator.IsAllocated(*frame_id));
    allocated.insert(*frame_id);
  }

  EXPECT_EQ(allocated.size(), 4U);
  EXPECT_EQ(allocator.allocated_count(), 4U);
  EXPECT_EQ(allocator.free_count(), 0U);
  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);
}

TEST(FrameAllocatorTest, FreeFrameMakesFrameAvailableAgain) {
  FrameAllocator allocator(4);

  const FrameId frame_id = *allocator.AllocateFrame();

  EXPECT_TRUE(allocator.IsAllocated(frame_id));
  EXPECT_EQ(allocator.allocated_count(), 1U);

  allocator.FreeFrame(frame_id);

  EXPECT_FALSE(allocator.IsAllocated(frame_id));
  EXPECT_EQ(allocator.allocated_count(), 0U);
  EXPECT_EQ(allocator.free_count(), 4U);

  const std::optional<FrameId> reused_frame_id = allocator.AllocateFrame();

  ASSERT_TRUE(reused_frame_id.has_value());
  EXPECT_EQ(*reused_frame_id, frame_id);
}

TEST(FrameAllocatorTest, DoubleFreeThrows) {
  FrameAllocator allocator(2);

  const FrameId frame_id = *allocator.AllocateFrame();
  allocator.FreeFrame(frame_id);

  EXPECT_THROW(allocator.FreeFrame(frame_id), std::logic_error);
}

TEST(FrameAllocatorTest, InvalidFrameIdThrows) {
  FrameAllocator allocator(2);

  EXPECT_THROW(static_cast<void>(allocator.IsAllocated(2)), std::out_of_range);
  EXPECT_THROW(allocator.FreeFrame(2), std::out_of_range);
  EXPECT_THROW(static_cast<void>(allocator.page(2)), std::out_of_range);
}

TEST(FrameAllocatorTest, PageAccessRequiresAllocatedFrame) {
  FrameAllocator allocator(2);

  EXPECT_THROW(static_cast<void>(allocator.page(0)), std::logic_error);

  const FrameId frame_id = *allocator.AllocateFrame();

  EXPECT_NO_THROW(static_cast<void>(allocator.page(frame_id)));

  allocator.FreeFrame(frame_id);

  EXPECT_THROW(static_cast<void>(allocator.page(frame_id)), std::logic_error);
}

TEST(FrameAllocatorTest, HandlesFrameCountNotDivisibleByWordSize) {
  constexpr std::size_t kFrameCount = 65;

  FrameAllocator allocator(kFrameCount);
  std::unordered_set<FrameId> allocated;

  for (std::size_t i = 0; i < kFrameCount; ++i) {
    std::optional<FrameId> frame_id = allocator.AllocateFrame();

    ASSERT_TRUE(frame_id.has_value());
    EXPECT_LT(*frame_id, kFrameCount);
    allocated.insert(*frame_id);
  }

  EXPECT_EQ(allocated.size(), kFrameCount);
  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);
}

TEST(FrameAllocatorTest, HandlesSingleFrame) {
  FrameAllocator allocator(1);

  const std::optional<FrameId> frame_id = allocator.AllocateFrame();

  ASSERT_TRUE(frame_id.has_value());
  EXPECT_EQ(*frame_id, 0U);
  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);

  allocator.FreeFrame(*frame_id);

  EXPECT_EQ(allocator.free_count(), 1U);
}

TEST(FrameAllocatorTest, AllocatedPagesArePageAligned) {
  FrameAllocator allocator(16);

  for (std::size_t i = 0; i < allocator.frame_count(); ++i) {
    const std::optional<FrameId> frame_id = allocator.AllocateFrame();

    ASSERT_TRUE(frame_id.has_value());
    EXPECT_TRUE(IsPageAligned(allocator.page(*frame_id)));
  }
}

TEST(FrameAllocatorTest, PageReferencesAreStable) {
  FrameAllocator allocator(4);

  const FrameId frame_id = *allocator.AllocateFrame();
  Page& page = allocator.page(frame_id);
  Page* page_pointer = &page;

  page.data[0] = std::byte{42};

  EXPECT_EQ(&allocator.page(frame_id), page_pointer);
  EXPECT_EQ(allocator.page(frame_id).data[0], std::byte{42});
}

TEST(FrameAllocatorTest, CanAllocateAfterFreeingManyFrames) {
  FrameAllocator allocator(128);
  std::vector<FrameId> frames;

  frames.reserve(allocator.frame_count());

  for (std::size_t i = 0; i < allocator.frame_count(); ++i) {
    frames.push_back(*allocator.AllocateFrame());
  }

  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);

  for (FrameId frame_id : frames) {
    allocator.FreeFrame(frame_id);
  }

  EXPECT_EQ(allocator.allocated_count(), 0U);
  EXPECT_EQ(allocator.free_count(), allocator.frame_count());

  for (std::size_t i = 0; i < allocator.frame_count(); ++i) {
    EXPECT_TRUE(allocator.AllocateFrame().has_value());
  }

  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);
}

TEST(FrameAllocatorTest, ReusesFreedFramesAcrossBitmapWords) {
  constexpr std::size_t kFrameCount = 130;
  FrameAllocator allocator(kFrameCount);

  std::vector<FrameId> frames;
  frames.reserve(kFrameCount);

  for (std::size_t i = 0; i < kFrameCount; ++i) {
    std::optional<FrameId> frame_id = allocator.AllocateFrame();

    ASSERT_TRUE(frame_id.has_value());
    ASSERT_LT(*frame_id, kFrameCount);

    frames.push_back(*frame_id);
  }

  EXPECT_EQ(allocator.allocated_count(), kFrameCount);
  EXPECT_EQ(allocator.free_count(), 0U);
  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);

  // Frames around 64-bit word boundaries
  constexpr FrameId kFrameInWord0 = 63;
  constexpr FrameId kFrameInWord1Low = 64;
  constexpr FrameId kFrameInWord1High = 127;
  constexpr FrameId kFrameInTailWord = 129;

  allocator.FreeFrame(kFrameInWord1High);
  allocator.FreeFrame(kFrameInWord0);
  allocator.FreeFrame(kFrameInTailWord);
  allocator.FreeFrame(kFrameInWord1Low);

  EXPECT_FALSE(allocator.IsAllocated(kFrameInWord0));
  EXPECT_FALSE(allocator.IsAllocated(kFrameInWord1Low));
  EXPECT_FALSE(allocator.IsAllocated(kFrameInWord1High));
  EXPECT_FALSE(allocator.IsAllocated(kFrameInTailWord));

  EXPECT_EQ(allocator.allocated_count(), kFrameCount - 4);
  EXPECT_EQ(allocator.free_count(), 4U);

  std::unordered_set<FrameId> reused;

  for (std::size_t i = 0; i < 4; ++i) {
    std::optional<FrameId> frame_id = allocator.AllocateFrame();

    ASSERT_TRUE(frame_id.has_value());
    EXPECT_LT(*frame_id, kFrameCount);

    reused.insert(*frame_id);
  }

  EXPECT_EQ(reused.size(), 4U);
  EXPECT_TRUE(reused.contains(kFrameInWord0));
  EXPECT_TRUE(reused.contains(kFrameInWord1Low));
  EXPECT_TRUE(reused.contains(kFrameInWord1High));
  EXPECT_TRUE(reused.contains(kFrameInTailWord));

  EXPECT_EQ(allocator.allocated_count(), kFrameCount);
  EXPECT_EQ(allocator.free_count(), 0U);
  EXPECT_EQ(allocator.AllocateFrame(), std::nullopt);
}

}  // namespace
}  // namespace buffer_manager