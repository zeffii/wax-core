// What one screen asks of the Pattern screen: which lane's tab opens, where its cursor goes, and how the grid scrolls to show it.
// Everything in it is a one-shot message. The screen that makes the request fills it in; the Pattern screen takes it on its next frame,
// after which nothing is pending. Keeping these in one place (and not as loose flags) is what keeps the Song and Pattern screens agreeing
// about where the cursor is and what is shown.
#pragma once

#include <string>

namespace wax::app {

struct Handoff {
  // The lane tab to open, and the tick the cursor goes to (-1: leave the cursor's row alone).
  void openLane(const std::string& laneId, int cursorTick = -1) { lane_ = laneId; tick_ = cursorTick; }
  void dropLaneRequest() { lane_.clear(); tick_ = -1; }
  bool laneRequested() const { return !lane_.empty(); }
  bool isRequested(const std::string& laneId) const { return !lane_.empty() && lane_ == laneId; }
  const std::string& requestedLane() const { return lane_; }
  int requestedTick() const { return tick_; }

  // The Pattern grid has opened the lane's tab and put the cursor where it was asked.
  void landed() { dropLaneRequest(); }

  // The grid scrolls so the cursor is in view on its next frame.
  void scrollToCursor() { scroll_ = true; }

  // Enter on a pattern in the sequence: the rows firstRow..endRow-1 are the pattern entered, and the grid brings all of them into view
  // (when they fit; otherwise from the first row), not just the cursor's row.
  void revealRows(int firstRow, int endRow) { revealFrom_ = firstRow; revealTo_ = endRow; }
  void forgetReveal() { revealFrom_ = revealTo_ = -1; }

  // What the grid is to do this frame, handed over once.
  struct Scroll {
    bool toCursor = false;
    int revealFrom = -1, revealTo = -1;
  };
  Scroll takeScroll() {
    Scroll out{scroll_, scroll_ ? revealFrom_ : -1, scroll_ ? revealTo_ : -1};
    if (scroll_) { scroll_ = false; forgetReveal(); }
    return out;
  }

 private:
  std::string lane_;
  int tick_ = -1;
  bool scroll_ = false;
  int revealFrom_ = -1, revealTo_ = -1;
};

}  // namespace wax::app
