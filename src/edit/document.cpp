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

namespace {

GraphNode& findGraphNode(Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return node;
  }
  throw std::out_of_range("no node " + nodeId + " in " + instrumentId);
}

}  // namespace

namespace {

Lane& findLane(Project& project, const std::string& laneId) {
  for (Lane& lane : project.lanes) if (lane.id == laneId) return lane;
  throw std::out_of_range("no lane " + laneId);
}

}  // namespace

SetDrawing::SetDrawing(std::string instrument, std::string node, std::string name, Json points)
    : instrument_(std::move(instrument)), node_(std::move(node)), name_(std::move(name)), points_(std::move(points)) {}

void SetDrawing::apply(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (!captured_) {
    hadData_ = node.data.is_object();
    beforeData_ = node.data;
    captured_ = true;
  }
  if (!node.data.is_object()) node.data = Json::object();
  if (!node.data.contains("env") || !node.data["env"].is_object()) node.data["env"] = Json::object();
  node.data["env"][name_] = points_;
}

void SetDrawing::revert(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  node.data = beforeData_;   // the whole data as it was before the first change
}

bool SetDrawing::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const SetDrawing*>(&next);
  if (!other || other->dataNode() != dataNode() || other->name_ != name_) return false;
  points_ = other->points_;
  return true;
}

namespace {
Graph& graphOf(Project& project, const std::string& instrumentId) {
  for (Instrument& instrument : project.instruments) if (instrument.id == instrumentId) return instrument.graph;
  throw std::out_of_range("no instrument " + instrumentId);
}
}  // namespace

void Connect::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  added_ = false;
  for (const Connection& c : g.connections) if (c.from == from_ && c.to == to_) return;
  g.connections.push_back({from_, to_});
  added_ = true;
}

void Connect::revert(Project& project) {
  if (!added_) return;
  Graph& g = graphOf(project, instrument_);
  for (std::size_t i = g.connections.size(); i-- > 0;) {
    if (g.connections[i].from == from_ && g.connections[i].to == to_) { g.connections.erase(g.connections.begin() + static_cast<std::ptrdiff_t>(i)); return; }
  }
}

void Disconnect::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  removed_ = false;
  for (std::size_t i = 0; i < g.connections.size(); i++) {
    if (g.connections[i].from == from_ && g.connections[i].to == to_) { at_ = i; g.connections.erase(g.connections.begin() + static_cast<std::ptrdiff_t>(i)); removed_ = true; return; }
  }
}

void Disconnect::revert(Project& project) {
  if (!removed_) return;
  Graph& g = graphOf(project, instrument_);
  g.connections.insert(g.connections.begin() + static_cast<std::ptrdiff_t>(std::min(at_, g.connections.size())), Connection{from_, to_});
}

void SetLoop::apply(Project& project) {
  if (!captured_) { before_ = project.song.raw.contains("loop") ? project.song.raw["loop"] : Json(); captured_ = true; }
  if (loop_.is_null()) project.song.raw.erase("loop"); else project.song.raw["loop"] = loop_;
}

void SetLoop::revert(Project& project) {
  if (before_.is_null()) project.song.raw.erase("loop"); else project.song.raw["loop"] = before_;
}

void AddNode::apply(Project& project) {
  GraphNode node;
  node.id = id_;
  node.type = type_;
  node.params = Json::object();
  node.x = x_; node.y = y_; node.placed = true;
  graphOf(project, instrument_).nodes.push_back(std::move(node));
}

void AddNode::revert(Project& project) {
  Graph& g = graphOf(project, instrument_);
  for (std::size_t i = g.nodes.size(); i-- > 0;) if (g.nodes[i].id == id_) { g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(i)); return; }
}

void DeleteNode::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  wires_.clear();
  for (std::size_t i = 0; i < g.nodes.size(); i++) {
    if (g.nodes[i].id == node_) { saved_ = g.nodes[i]; nodeAt_ = i; g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(i)); break; }
  }
  const auto touches = [&](const std::string& address) { return address.substr(0, address.find('.')) == node_; };
  for (std::size_t i = g.connections.size(); i-- > 0;) {
    if (touches(g.connections[i].from) || touches(g.connections[i].to)) {
      wires_.insert(wires_.begin(), {i, g.connections[i]});
      g.connections.erase(g.connections.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
}

void DeleteNode::revert(Project& project) {
  Graph& g = graphOf(project, instrument_);
  g.nodes.insert(g.nodes.begin() + static_cast<std::ptrdiff_t>(std::min(nodeAt_, g.nodes.size())), saved_);
  for (const auto& [at, c] : wires_) g.connections.insert(g.connections.begin() + static_cast<std::ptrdiff_t>(std::min(at, g.connections.size())), c);
}

MoveNode::MoveNode(std::string instrument, std::string node, double x, double y)
    : instrument_(std::move(instrument)), node_(std::move(node)), x_(x), y_(y) {}

void MoveNode::apply(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (!captured_) { beforeX_ = node.x; beforeY_ = node.y; beforePlaced_ = node.placed; captured_ = true; }
  node.x = x_; node.y = y_; node.placed = true;
}

void MoveNode::revert(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  node.x = beforeX_; node.y = beforeY_; node.placed = beforePlaced_;
}

bool MoveNode::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const MoveNode*>(&next);
  if (!other || other->instrument_ != instrument_ || other->node_ != node_) return false;
  x_ = other->x_; y_ = other->y_;
  return true;
}

SetBank::SetBank(std::string instrument, std::string node, std::vector<int> bytes, std::string what)
    : instrument_(std::move(instrument)), node_(std::move(node)), what_(std::move(what)), bytes_(std::move(bytes)) {}

void SetBank::apply(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (!captured_) { beforeData_ = node.data; captured_ = true; }
  if (!node.data.is_object()) node.data = Json::object();
  node.data["bank"] = bytes_;
}

void SetBank::revert(Project& project) { findGraphNode(project, instrument_, node_).data = beforeData_; }

bool SetBank::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const SetBank*>(&next);
  if (!other || other->dataNode() != dataNode()) return false;
  bytes_ = other->bytes_;
  return true;
}

void SetSilenced::apply(Project& project) {
  Lane& lane = findLane(project, lane_);
  before_ = lane.silenced;
  lane.silenced = silenced_;
}

void SetSilenced::revert(Project& project) { findLane(project, lane_).silenced = before_; }

SetParam::SetParam(std::string instrument, std::string node, std::string param, double value)
    : instrument_(std::move(instrument)), node_(std::move(node)), param_(std::move(param)), value_(value) {}

void SetParam::apply(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (!node.params.is_object()) node.params = Json::object();
  if (!captured_) {
    had_ = node.params.contains(param_);
    if (had_) before_ = node.params[param_];
    captured_ = true;
  }
  node.params[param_] = value_;
}

void SetParam::revert(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (had_) node.params[param_] = before_;
  else node.params.erase(param_);
}

bool SetParam::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const SetParam*>(&next);
  if (!other || other->paramAddress() != paramAddress()) return false;
  value_ = other->value_;
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
  if (!edit.paramAddress().empty()) changedParams_.insert(edit.paramAddress());
  if (!edit.dataNode().empty()) changedData_.insert(edit.dataNode());
  if (edit.songLoop()) loopChanged_ = true;
  if (!edit.graphInstrument().empty()) changedGraphs_.insert(edit.graphInstrument());
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

std::set<std::string> Document::takeChangedGraphs() {
  std::set<std::string> out;
  out.swap(changedGraphs_);
  return out;
}

std::set<std::string> Document::takeChangedData() {
  std::set<std::string> out;
  out.swap(changedData_);
  return out;
}

std::set<std::string> Document::takeChangedParams() {
  std::set<std::string> out;
  out.swap(changedParams_);
  return out;
}

std::set<std::string> Document::takeChangedLanes() {
  std::set<std::string> out;
  out.swap(changed_);
  return out;
}

}  // namespace wax
