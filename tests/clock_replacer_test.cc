#include "replacement/clock_replacer.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

namespace buffer_manager {
namespace {

TEST(ClockReplacerTest, ConstructorRejectsZeroFrames) {
  EXPECT_THROW(ClockReplacer(0), std::invalid_argument);
}

TEST(ClockReplacerTest, EmptyReplacerHasNoVictim) {
  ClockReplacer replacer(4);

  EXPECT_EQ(replacer.frame_count(), 4U);
  EXPECT_EQ(replacer.evictable_count(), 0U);
  EXPECT_EQ(replacer.Victim(), std::nullopt);
}

TEST(ClockReplacerTest, RecordAccessDoesNotMakeFrameEvictable) {
  ClockReplacer replacer(4);

  replacer.RecordAccess(1);

  EXPECT_EQ(replacer.evictable_count(), 0U);
  EXPECT_EQ(replacer.Victim(), std::nullopt);
}

TEST(ClockReplacerTest, SetEvictableRequiresKnownFrame) {
  ClockReplacer replacer(4);

  EXPECT_THROW(replacer.SetEvictable(1, true), std::logic_error);
}

TEST(ClockReplacerTest, VictimReturnsOnlyEvictableFrames) {
  ClockReplacer replacer(4);

  replacer.RecordAccess(0);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);

  replacer.SetEvictable(0, false);
  replacer.SetEvictable(1, true);
  replacer.SetEvictable(2, false);

  EXPECT_EQ(replacer.evictable_count(), 1U);
  EXPECT_EQ(replacer.Victim(), std::optional<FrameId>{1});
  EXPECT_EQ(replacer.evictable_count(), 0U);
  EXPECT_EQ(replacer.Victim(), std::nullopt);
}

TEST(ClockReplacerTest, RecentlyAccessedFrameGetsSecondChance) {
  ClockReplacer replacer(3);

  replacer.RecordAccess(0);
  replacer.SetEvictable(0, true);

  replacer.RecordAccess(1);
  replacer.SetEvictable(1, true);

  replacer.RecordAccess(2);
  replacer.SetEvictable(2, true);

  EXPECT_EQ(replacer.Victim(), std::optional<FrameId>{0});

  replacer.RecordAccess(0);
  replacer.SetEvictable(0, true);

  // Frame one is touched, should be given a second chance
  replacer.RecordAccess(1);

  EXPECT_EQ(replacer.Victim(), std::optional<FrameId>{2});

  // Second chance for frame 0
  EXPECT_EQ(replacer.Victim(), std::optional<FrameId>{1});
}

TEST(ClockReplacerTest, SetEvictableMaintainsCount) {
  ClockReplacer replacer(4);

  replacer.RecordAccess(2);

  EXPECT_EQ(replacer.evictable_count(), 0U);

  replacer.SetEvictable(2, true);
  EXPECT_EQ(replacer.evictable_count(), 1U);

  replacer.SetEvictable(2, true);
  EXPECT_EQ(replacer.evictable_count(), 1U);

  replacer.SetEvictable(2, false);
  EXPECT_EQ(replacer.evictable_count(), 0U);

  replacer.SetEvictable(2, false);
  EXPECT_EQ(replacer.evictable_count(), 0U);
}

TEST(ClockReplacerTest, RemoveClearsFrameAndUpdatesCount) {
  ClockReplacer replacer(4);

  replacer.RecordAccess(3);
  replacer.SetEvictable(3, true);

  EXPECT_EQ(replacer.evictable_count(), 1U);

  replacer.Remove(3);

  EXPECT_EQ(replacer.evictable_count(), 0U);
  EXPECT_EQ(replacer.Victim(), std::nullopt);

  // Removing an already absent frame is allowed
  replacer.Remove(3);
  EXPECT_EQ(replacer.evictable_count(), 0U);
}

TEST(ClockReplacerTest, VictimRemovesReturnedFrame) {
  ClockReplacer replacer(4);

  replacer.RecordAccess(0);
  replacer.SetEvictable(0, true);

  replacer.RecordAccess(1);
  replacer.SetEvictable(1, true);

  const std::optional<FrameId> first = replacer.Victim();
  const std::optional<FrameId> second = replacer.Victim();

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  EXPECT_NE(*first, *second);
  EXPECT_EQ(replacer.evictable_count(), 0U);
  EXPECT_EQ(replacer.Victim(), std::nullopt);
}

TEST(ClockReplacerTest, VictimSkipsAbsentAndNonEvictableFrames) {
  ClockReplacer replacer(5);

  replacer.RecordAccess(0);
  replacer.SetEvictable(0, false);

  replacer.RecordAccess(3);
  replacer.SetEvictable(3, true);

  EXPECT_EQ(replacer.Victim(), std::optional<FrameId>{3});
}

TEST(ClockReplacerTest, InvalidFrameIdThrows) {
  ClockReplacer replacer(2);

  EXPECT_THROW(replacer.RecordAccess(2), std::out_of_range);
  EXPECT_THROW(replacer.SetEvictable(2, true), std::out_of_range);
  EXPECT_THROW(replacer.Remove(2), std::out_of_range);
}

}  // namespace
}  // namespace buffer_manager