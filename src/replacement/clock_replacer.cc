#include "replacement/clock_replacer.h"

#include <limits>
#include <stdexcept>

namespace buffer_manager {

ClockReplacer::ClockReplacer(std::size_t frame_count) {
  if (frame_count == 0) {
    throw std::invalid_argument("frame_count must be greater than zero");
  }

  if (frame_count > std::numeric_limits<FrameId>::max()) {
    throw std::invalid_argument("frame_count does not fit into FrameId");
  }

  entries_.resize(frame_count);
}

void ClockReplacer::RecordAccess(FrameId frame_id) {
  ValidateFrameId(frame_id);

  Entry& entry = entries_[static_cast<std::size_t>(frame_id)];
  entry.present = true;
  entry.referenced = true;
}

void ClockReplacer::SetEvictable(FrameId frame_id, bool evictable) {
  ValidateFrameId(frame_id);

  Entry& entry = entries_[static_cast<std::size_t>(frame_id)];

  if (!entry.present) {
    throw std::logic_error("cannot change evictability of an unknown frame");
  }

  if (entry.evictable == evictable) {
    return;
  }

  entry.evictable = evictable;

  if (evictable) {
    ++evictable_count_;
  } else {
    --evictable_count_;
  }
}

std::optional<FrameId> ClockReplacer::Victim() {
  if (evictable_count_ == 0) {
    return std::nullopt;
  }

  for (std::size_t pass = 0; pass < 2; ++pass) {
    for (std::size_t scanned = 0; scanned < entries_.size(); ++scanned) {
      Entry& entry = entries_[clock_hand_];

      if (entry.present && entry.evictable) {
        if (entry.referenced) {
          entry.referenced = false;
        } else {
          const auto victim = static_cast<FrameId>(clock_hand_);
          Remove(victim);
          AdvanceClockHand();
          return victim;
        }
      }

      AdvanceClockHand();
    }
  }

  return std::nullopt;
}

void ClockReplacer::Remove(FrameId frame_id) {
  ValidateFrameId(frame_id);

  Entry& entry = entries_[static_cast<std::size_t>(frame_id)];

  if (!entry.present) {
    return;
  }

  if (entry.evictable) {
    --evictable_count_;
  }

  entry = Entry{};
}

std::size_t ClockReplacer::frame_count() const noexcept {
  return entries_.size();
}

std::size_t ClockReplacer::evictable_count() const noexcept {
  return evictable_count_;
}

void ClockReplacer::ValidateFrameId(FrameId frame_id) const {
  if (static_cast<std::size_t>(frame_id) >= entries_.size()) {
    throw std::out_of_range("invalid frame id");
  }
}

void ClockReplacer::AdvanceClockHand() noexcept {
  ++clock_hand_;

  if (clock_hand_ == entries_.size()) {
    clock_hand_ = 0;
  }
}

}  // namespace buffer_manager