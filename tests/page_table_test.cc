#include "page/page_table.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace buffer_manager {
namespace {

TEST(PageTableTest, ConstructorRejectsZeroPages) {
  EXPECT_THROW(PageTable(0), std::invalid_argument);
}

TEST(PageTableTest, StartsWithNoAllocatedPages) {
  PageTable table(3);

  EXPECT_EQ(table.max_page_count(), 3U);
  EXPECT_EQ(table.allocated_count(), 0U);

  for (PageId page_id = 0; page_id < 3; ++page_id) {
    EXPECT_FALSE(table.IsAllocated(page_id));
    EXPECT_FALSE(table.IsResident(page_id));
    EXPECT_EQ(table.State(page_id), PageState::kUnused);
    EXPECT_THROW(static_cast<void>(table.FrameForPage(page_id)),
                 std::logic_error);
  }
}

TEST(PageTableTest, AllocatePageIdsAreUniqueAndStartNonResident) {
  PageTable table(3);
  std::unordered_set<PageId> page_ids;

  for (PageId expected = 0; expected < 3; ++expected) {
    const PageId page_id = table.AllocatePageId();

    EXPECT_EQ(page_id, expected);
    EXPECT_TRUE(page_ids.insert(page_id).second);

    EXPECT_TRUE(table.IsAllocated(page_id));
    EXPECT_FALSE(table.IsResident(page_id));
    EXPECT_EQ(table.State(page_id), PageState::kNonResident);
    EXPECT_EQ(table.FrameForPage(page_id), std::nullopt);
  }

  EXPECT_EQ(table.allocated_count(), 3U);
  EXPECT_THROW(static_cast<void>(table.AllocatePageId()), std::runtime_error);
}

TEST(PageTableTest, AllocatedCountTracksAllocatedPagesOnly) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  EXPECT_EQ(table.allocated_count(), 1U);

  table.SetResident(page_id, 2);
  EXPECT_EQ(table.allocated_count(), 1U);

  table.SetEvicting(page_id);
  EXPECT_EQ(table.allocated_count(), 1U);

  table.SetNonResident(page_id);
  EXPECT_EQ(table.allocated_count(), 1U);

  table.FreePageId(page_id);
  EXPECT_EQ(table.allocated_count(), 0U);
}

TEST(PageTableTest, FreedPageIdIsReused) {
  PageTable table(4);

  const PageId page0 = table.AllocatePageId();
  const PageId page1 = table.AllocatePageId();
  const PageId page2 = table.AllocatePageId();

  EXPECT_EQ(page0, 0U);
  EXPECT_EQ(page1, 1U);
  EXPECT_EQ(page2, 2U);

  table.FreePageId(page1);

  EXPECT_FALSE(table.IsAllocated(page1));
  EXPECT_EQ(table.State(page1), PageState::kUnused);
  EXPECT_EQ(table.allocated_count(), 2U);

  const PageId reused_page = table.AllocatePageId();

  EXPECT_EQ(reused_page, page1);
  EXPECT_TRUE(table.IsAllocated(reused_page));
  EXPECT_EQ(table.State(reused_page), PageState::kNonResident);
  EXPECT_EQ(table.FrameForPage(reused_page), std::nullopt);
}

TEST(PageTableTest, DoubleFreeThrows) {
  PageTable table(2);

  const PageId page_id = table.AllocatePageId();

  table.FreePageId(page_id);

  EXPECT_THROW(table.FreePageId(page_id), std::logic_error);
}

TEST(PageTableTest, InvalidPageIdThrows) {
  PageTable table(2);

  EXPECT_THROW(static_cast<void>(table.IsAllocated(2)), std::out_of_range);
  EXPECT_THROW(static_cast<void>(table.IsResident(2)), std::out_of_range);
  EXPECT_THROW(static_cast<void>(table.State(2)), std::out_of_range);
  EXPECT_THROW(static_cast<void>(table.FrameForPage(2)), std::out_of_range);

  EXPECT_THROW(table.FreePageId(2), std::out_of_range);
  EXPECT_THROW(table.SetLoading(2, 0), std::out_of_range);
  EXPECT_THROW(table.SetResident(2, 0), std::out_of_range);
  EXPECT_THROW(table.SetNonResident(2), std::out_of_range);
  EXPECT_THROW(table.SetEvicting(2), std::out_of_range);
}

TEST(PageTableTest, FrameForUnusedPageThrows) {
  PageTable table(2);

  EXPECT_THROW(static_cast<void>(table.FrameForPage(0)), std::logic_error);
}

TEST(PageTableTest, SetResidentFromNonResidentStoresFrameId) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetResident(page_id, 7);

  EXPECT_TRUE(table.IsAllocated(page_id));
  EXPECT_TRUE(table.IsResident(page_id));
  EXPECT_EQ(table.State(page_id), PageState::kResident);
  EXPECT_EQ(table.FrameForPage(page_id), std::optional<FrameId>{7});
}

TEST(PageTableTest, SetNonResidentClearsFrameId) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetResident(page_id, 3);
  table.SetNonResident(page_id);

  EXPECT_TRUE(table.IsAllocated(page_id));
  EXPECT_FALSE(table.IsResident(page_id));
  EXPECT_EQ(table.State(page_id), PageState::kNonResident);
  EXPECT_EQ(table.FrameForPage(page_id), std::nullopt);
}

TEST(PageTableTest, SetLoadingStoresFrameIdButIsNotResident) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetLoading(page_id, 5);

  EXPECT_TRUE(table.IsAllocated(page_id));
  EXPECT_FALSE(table.IsResident(page_id));
  EXPECT_EQ(table.State(page_id), PageState::kLoading);
  EXPECT_EQ(table.FrameForPage(page_id), std::optional<FrameId>{5});
}

TEST(PageTableTest, CannotStartSecondLoadForLoadingPage) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetLoading(page_id, 5);

  EXPECT_THROW(table.SetLoading(page_id, 6), std::logic_error);
}

TEST(PageTableTest, SetResidentPublishesLoadingPage) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetLoading(page_id, 5);
  table.SetResident(page_id, 5);

  EXPECT_TRUE(table.IsResident(page_id));
  EXPECT_EQ(table.State(page_id), PageState::kResident);
  EXPECT_EQ(table.FrameForPage(page_id), std::optional<FrameId>{5});
}

TEST(PageTableTest, SetResidentRejectsLoadingFrameMismatch) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetLoading(page_id, 5);

  EXPECT_THROW(table.SetResident(page_id, 6), std::logic_error);
}

TEST(PageTableTest, SetNonResidentFromLoadingClearsFrameId) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetLoading(page_id, 5);
  table.SetNonResident(page_id);

  EXPECT_FALSE(table.IsResident(page_id));
  EXPECT_EQ(table.State(page_id), PageState::kNonResident);
  EXPECT_EQ(table.FrameForPage(page_id), std::nullopt);
}

TEST(PageTableTest, SetEvictingRequiresResidentPage) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  EXPECT_THROW(table.SetEvicting(page_id), std::logic_error);

  table.SetLoading(page_id, 1);
  EXPECT_THROW(table.SetEvicting(page_id), std::logic_error);

  table.SetResident(page_id, 1);
  EXPECT_NO_THROW(table.SetEvicting(page_id));

  EXPECT_EQ(table.State(page_id), PageState::kEvicting);
  EXPECT_FALSE(table.IsResident(page_id));
  EXPECT_EQ(table.FrameForPage(page_id), std::optional<FrameId>{1});
}

TEST(PageTableTest, FreePageIdRequiresNonResidentPage) {
  PageTable table(4);

  const PageId resident_page = table.AllocatePageId();
  table.SetResident(resident_page, 1);
  EXPECT_THROW(table.FreePageId(resident_page), std::logic_error);

  const PageId loading_page = table.AllocatePageId();
  table.SetLoading(loading_page, 2);
  EXPECT_THROW(table.FreePageId(loading_page), std::logic_error);

  const PageId evicting_page = table.AllocatePageId();
  table.SetResident(evicting_page, 3);
  table.SetEvicting(evicting_page);
  EXPECT_THROW(table.FreePageId(evicting_page), std::logic_error);
}

TEST(PageTableTest, FreePageIdClearsFrameAndReusesPageId) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetResident(page_id, 1);
  table.SetNonResident(page_id);
  table.FreePageId(page_id);

  EXPECT_FALSE(table.IsAllocated(page_id));
  EXPECT_EQ(table.State(page_id), PageState::kUnused);
  EXPECT_THROW(static_cast<void>(table.FrameForPage(page_id)),
               std::logic_error);

  const PageId reused_page_id = table.AllocatePageId();

  EXPECT_EQ(reused_page_id, page_id);
  EXPECT_TRUE(table.IsAllocated(reused_page_id));
  EXPECT_EQ(table.State(reused_page_id), PageState::kNonResident);
}

TEST(PageTableTest, InvalidTransitionsFromUnusedPageThrow) {
  PageTable table(4);

  EXPECT_THROW(table.SetLoading(0, 1), std::logic_error);
  EXPECT_THROW(table.SetResident(0, 1), std::logic_error);
  EXPECT_THROW(table.SetNonResident(0), std::logic_error);
  EXPECT_THROW(table.SetEvicting(0), std::logic_error);
}

TEST(PageTableTest, InvalidResidentTransitionsThrow) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetResident(page_id, 1);

  EXPECT_THROW(table.SetResident(page_id, 1), std::logic_error);
  EXPECT_THROW(table.SetLoading(page_id, 1), std::logic_error);
}

TEST(PageTableTest, CannotMakeEvictingPageResidentDirectly) {
  PageTable table(4);

  const PageId page_id = table.AllocatePageId();

  table.SetResident(page_id, 1);
  table.SetEvicting(page_id);

  EXPECT_THROW(table.SetResident(page_id, 1), std::logic_error);
}

}  // namespace
}  // namespace buffer_manager