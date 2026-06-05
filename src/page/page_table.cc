#include "page/page_table.h"

#include <limits>
#include <stdexcept>

namespace buffer_manager {

PageTable::PageTable(PageId max_page_count) : max_page_count_(max_page_count) {
  if (max_page_count == 0) {
    throw std::invalid_argument("max_page_count must be greater than zero");
  }

  if (max_page_count > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("max_page_count does not fit into size_t");
  }

  pages_.resize(static_cast<std::size_t>(max_page_count));
}

PageId PageTable::AllocatePageId() {
  PageId page_id = 0;

  if (!free_page_ids_.empty()) {
    page_id = free_page_ids_.back();
    free_page_ids_.pop_back();
  } else {
    if (next_page_id_ == max_page_count_) {
      throw std::runtime_error("maximum number of pages reached");
    }

    page_id = next_page_id_;
    ++next_page_id_;
  }

  Entry& entry = EntryFor(page_id);
  entry.state = PageState::kNonResident;
  entry.frame_id = std::nullopt;

  ++allocated_count_;
  return page_id;
}

void PageTable::FreePageId(PageId page_id) {
  Entry& entry = EntryFor(page_id);

  if (entry.state == PageState::kUnused) {
    throw std::logic_error("cannot free an unused page");
  }

  if (entry.state != PageState::kNonResident) {
    throw std::logic_error("cannot free a page that is still memory-managed");
  }

  entry = Entry{};
  free_page_ids_.push_back(page_id);
  --allocated_count_;
}

bool PageTable::IsAllocated(PageId page_id) const {
  return EntryFor(page_id).state != PageState::kUnused;
}

bool PageTable::IsResident(PageId page_id) const {
  return EntryFor(page_id).state == PageState::kResident;
}

PageState PageTable::State(PageId page_id) const {
  return EntryFor(page_id).state;
}

std::optional<FrameId> PageTable::FrameForPage(PageId page_id) const {
  const Entry& entry = EntryFor(page_id);

  if (entry.state == PageState::kUnused) {
    throw std::logic_error("unused page has no frame");
  }

  return entry.frame_id;
}

void PageTable::SetLoading(PageId page_id, FrameId frame_id) {
  Entry& entry = EntryFor(page_id);

  if (entry.state == PageState::kUnused) {
    throw std::logic_error("cannot load an unused page");
  }

  if (entry.state != PageState::kNonResident) {
    throw std::logic_error("page must be nonresident before loading");
  }

  entry.state = PageState::kLoading;
  entry.frame_id = frame_id;
}

void PageTable::SetResident(PageId page_id, FrameId frame_id) {
  Entry& entry = EntryFor(page_id);

  if (entry.state == PageState::kUnused) {
    throw std::logic_error("cannot make an unused page resident");
  }

  if (entry.state == PageState::kResident) {
    throw std::logic_error("page is already resident");
  }

  if (entry.state == PageState::kEvicting) {
    throw std::logic_error("cannot make an evicting page resident");
  }

  if (entry.state == PageState::kLoading && entry.frame_id != frame_id) {
    throw std::logic_error("loading page frame mismatch");
  }

  entry.state = PageState::kResident;
  entry.frame_id = frame_id;
}

void PageTable::SetNonResident(PageId page_id) {
  Entry& entry = EntryFor(page_id);

  if (entry.state == PageState::kUnused) {
    throw std::logic_error("cannot make an unused page nonresident");
  }

  entry.state = PageState::kNonResident;
  entry.frame_id = std::nullopt;
}

void PageTable::SetEvicting(PageId page_id) {
  Entry& entry = EntryFor(page_id);

  if (entry.state == PageState::kUnused) {
    throw std::logic_error("cannot evict an unused page");
  }

  if (entry.state != PageState::kResident) {
    throw std::logic_error("only resident pages can become evicting");
  }

  entry.state = PageState::kEvicting;
}

PageId PageTable::max_page_count() const noexcept {
  return max_page_count_;
}

PageId PageTable::allocated_count() const noexcept {
  return allocated_count_;
}

void PageTable::ValidatePageId(PageId page_id) const {
  if (page_id >= max_page_count_) {
    throw std::out_of_range("invalid page id");
  }
}

PageTable::Entry& PageTable::EntryFor(PageId page_id) {
  ValidatePageId(page_id);
  return pages_[static_cast<std::size_t>(page_id)];
}

const PageTable::Entry& PageTable::EntryFor(PageId page_id) const {
  ValidatePageId(page_id);
  return pages_[static_cast<std::size_t>(page_id)];
}

}  // namespace buffer_manager