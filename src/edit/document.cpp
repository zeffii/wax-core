#include "edit/document.h"

#include <sstream>
#include <stdexcept>

namespace wax {
namespace {

Pattern& findPattern(Project& project, const std::string& laneId, const std::string& patternId) {
  for (Lane& lane : project.lanes) {
    if (lane.id != laneId) continue;
    for (Pattern& pattern : lane.patterns) if (pattern.id == patternId) return pattern;
  }
  throw std::out_of_range("no pattern " + patternId + " in lane " + laneId);
}

std::string& findRow(Pattern& pattern, int row) {
  if (row < 0 || row >= static_cast<int>(pattern.rows.size())) throw std::out_of_range("no row " + std::to_string(row));
  return pattern.rows[static_cast<std::size_t>(row)];
}

}  // namespace

std::string withCell(const std::string& row, int cell, const std::string& text) {
  if (cell < 0) throw std::out_of_range("no cell " + std::to_string(cell));
  std::string out;
  std::size_t i = 0;
  int at = 0;
  while (i < row.size()) {
    if (row[i] == ' ' || row[i] == '\t') { out += row[i++]; continue; }
    std::size_t j = i;
    while (j < row.size() && row[j] != ' ' && row[j] != '\t') j++;
    out += at == cell ? text : row.substr(i, j - i);
    if (at == cell) { out += row.substr(j); return out; }
    at++;
    i = j;
  }
  throw std::out_of_range("no cell " + std::to_string(cell));
}

SetRow::SetRow(std::string lane, std::string pattern, int row, std::string text)
    : lane_(std::move(lane)), pattern_(std::move(pattern)), row_(row), text_(std::move(text)) {}

void SetRow::apply(Project& project) {
  std::string& line = findRow(findPattern(project, lane_, pattern_), row_);
  if (!applied_) before_ = line;
  line = text_;
  applied_ = true;
}

void SetRow::revert(Project& project) { findRow(findPattern(project, lane_, pattern_), row_) = before_; }

bool SetRow::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const SetRow*>(&next);
  if (!other || other->lane_ != lane_ || other->pattern_ != pattern_ || other->row_ != row_) return false;
  text_ = other->text_;   // `before_` stays: undo goes back to what the row was before the first step
  return true;
}

AddPattern::AddPattern(std::string lane, Pattern pattern, Placement placement)
    : lane_(std::move(lane)), pattern_(std::move(pattern)), placement_(std::move(placement)) {}

void AddPattern::apply(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    lane.patterns.push_back(pattern_);
    lane.placements.push_back(placement_);
    return;
  }
  throw std::out_of_range("no lane " + lane_);
}

void AddPattern::revert(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    for (std::size_t i = lane.patterns.size(); i-- > 0;) if (lane.patterns[i].id == pattern_.id) { lane.patterns.erase(lane.patterns.begin() + static_cast<long>(i)); break; }
    for (std::size_t i = lane.placements.size(); i-- > 0;) {
      if (lane.placements[i].pattern == pattern_.id && lane.placements[i].start == placement_.start) { lane.placements.erase(lane.placements.begin() + static_cast<long>(i)); break; }
    }
    return;
  }
}

bool AddPattern::absorb(const Edit& next) {
  const auto* row = dynamic_cast<const SetRow*>(&next);
  if (!row || row->lane() != lane_ || row->patternId() != pattern_.id) return false;
  if (row->row() < 0 || row->row() >= static_cast<int>(pattern_.rows.size())) return false;
  pattern_.rows[static_cast<std::size_t>(row->row())] = row->text();   // redo brings the pattern back with what was typed in it
  return true;
}

InsertRow::InsertRow(std::string lane, std::string pattern, int row, std::string blank)
    : lane_(std::move(lane)), pattern_(std::move(pattern)), row_(row), blank_(std::move(blank)) {}

void InsertRow::apply(Project& project) {
  Pattern& pattern = findPattern(project, lane_, pattern_);
  findRow(pattern, row_);
  dropped_ = pattern.rows.back();
  pattern.rows.pop_back();
  pattern.rows.insert(pattern.rows.begin() + row_, blank_);
}

void InsertRow::revert(Project& project) {
  Pattern& pattern = findPattern(project, lane_, pattern_);
  pattern.rows.erase(pattern.rows.begin() + row_);
  pattern.rows.push_back(dropped_);
}

DeleteRow::DeleteRow(std::string lane, std::string pattern, int row, std::string blank)
    : lane_(std::move(lane)), pattern_(std::move(pattern)), row_(row), blank_(std::move(blank)) {}

void DeleteRow::apply(Project& project) {
  Pattern& pattern = findPattern(project, lane_, pattern_);
  removed_ = findRow(pattern, row_);
  pattern.rows.erase(pattern.rows.begin() + row_);
  pattern.rows.push_back(blank_);
}

void DeleteRow::revert(Project& project) {
  Pattern& pattern = findPattern(project, lane_, pattern_);
  pattern.rows.pop_back();
  pattern.rows.insert(pattern.rows.begin() + row_, removed_);
}

void Document::touched(const Edit& edit) {
  revision_++;
  if (!edit.lane().empty()) changed_.insert(edit.lane());
}

void Document::apply(std::unique_ptr<Edit> edit, bool foldIntoPrevious) {
  redo_.clear();
  edit->apply(project_);
  touched(*edit);
  if (foldIntoPrevious && !undo_.empty() && undo_.back()->absorb(*edit)) return;
  undo_.push_back(std::move(edit));
}

void Document::undo() {
  if (undo_.empty()) return;
  undo_.back()->revert(project_);
  touched(*undo_.back());
  redo_.push_back(std::move(undo_.back()));
  undo_.pop_back();
}

void Document::redo() {
  if (redo_.empty()) return;
  redo_.back()->apply(project_);
  touched(*redo_.back());
  undo_.push_back(std::move(redo_.back()));
  redo_.pop_back();
}

std::set<std::string> Document::takeChangedLanes() {
  std::set<std::string> out;
  out.swap(changed_);
  return out;
}

}  // namespace wax
