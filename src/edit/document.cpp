#include <algorithm>
#include <set>
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

void AddInstrument::apply(Project& project) {
  project.instruments.push_back(instrumentFromJson(instrument_, definition_));
  project.lanes.push_back(blankLane(lane_, instrument_));
}

void AddInstrument::revert(Project& project) {
  for (std::size_t i = project.lanes.size(); i-- > 0;) if (project.lanes[i].id == lane_) { project.lanes.erase(project.lanes.begin() + static_cast<long>(i)); break; }
  for (std::size_t i = project.instruments.size(); i-- > 0;) if (project.instruments[i].id == instrument_) { project.instruments.erase(project.instruments.begin() + static_cast<long>(i)); break; }
}

namespace {
int laneIndex(const Project& project, const std::string& id) {
  for (std::size_t i = 0; i < project.lanes.size(); i++) if (project.lanes[i].id == id) return static_cast<int>(i);
  return -1;
}
}  // namespace

void RenameLane::apply(Project& project) {
  const int at = laneIndex(project, from_);
  if (at < 0) throw std::out_of_range("no lane " + from_);
  project.lanes[static_cast<std::size_t>(at)].id = to_;
  if (project.lanes[static_cast<std::size_t>(at)].raw.is_object()) project.lanes[static_cast<std::size_t>(at)].raw["id"] = to_;
}

void RenameLane::revert(Project& project) {
  const int at = laneIndex(project, to_);
  if (at < 0) return;
  project.lanes[static_cast<std::size_t>(at)].id = from_;
  if (project.lanes[static_cast<std::size_t>(at)].raw.is_object()) project.lanes[static_cast<std::size_t>(at)].raw["id"] = from_;
}

void MoveLane::apply(Project& project) {
  from_ = laneIndex(project, lane_);
  to_ = from_ + direction_;
  if (from_ < 0 || to_ < 0 || to_ >= static_cast<int>(project.lanes.size())) { from_ = to_ = -1; return; }
  if (project.lanes[static_cast<std::size_t>(from_)].id == "master" || project.lanes[static_cast<std::size_t>(to_)].id == "master") { from_ = to_ = -1; return; }   // the master stays first
  std::swap(project.lanes[static_cast<std::size_t>(from_)], project.lanes[static_cast<std::size_t>(to_)]);
}

void MoveLane::revert(Project& project) {
  if (from_ >= 0) std::swap(project.lanes[static_cast<std::size_t>(from_)], project.lanes[static_cast<std::size_t>(to_)]);
}

void DuplicateLane::apply(Project& project) {
  const int at = laneIndex(project, source_);
  if (at < 0) throw std::out_of_range("no lane " + source_);
  Lane copy = project.lanes[static_cast<std::size_t>(at)];
  copy.id = copy_;
  if (copy.raw.is_object()) copy.raw["id"] = copy_;
  project.lanes.insert(project.lanes.begin() + at + 1, std::move(copy));
}

void DuplicateLane::revert(Project& project) {
  const int at = laneIndex(project, copy_);
  if (at >= 0) project.lanes.erase(project.lanes.begin() + at);
}

void RemoveLane::apply(Project& project) {
  laneAt_ = laneIndex(project, lane_);
  if (laneAt_ < 0) throw std::out_of_range("no lane " + lane_);
  removed_ = project.lanes[static_cast<std::size_t>(laneAt_)];
  project.lanes.erase(project.lanes.begin() + laneAt_);
  instrumentAt_ = -1;
  if (!withMachine_) return;
  for (std::size_t i = 0; i < project.instruments.size(); i++) {
    if (project.instruments[i].id != removed_.instrument) continue;
    instrument_ = project.instruments[i];
    instrumentAt_ = static_cast<int>(i);
    project.instruments.erase(project.instruments.begin() + static_cast<long>(i));
    break;
  }
}

void RemoveLane::revert(Project& project) {
  if (instrumentAt_ >= 0) project.instruments.insert(project.instruments.begin() + instrumentAt_, instrument_);
  project.lanes.insert(project.lanes.begin() + laneAt_, removed_);
}

namespace {
Lane& laneNamed(Project& project, const std::string& id) {
  for (Lane& l : project.lanes) if (l.id == id) return l;
  throw std::out_of_range("no lane " + id);
}
}  // namespace

namespace {
constexpr int kMaxSong = 65536;
int movedBy(int t, int at, int n) { return t >= at ? t + n : t; }
}  // namespace

void SetSongInfo::apply(Project& project) {
  old_ = project.song.raw.contains("info") ? project.song.raw["info"] : Json();
  if (text_.empty()) project.song.raw.erase("info"); else project.song.raw["info"] = text_;
}

void SetSongInfo::revert(Project& project) {
  if (old_.is_null()) project.song.raw.erase("info"); else project.song.raw["info"] = old_;
}

void SetSongLength::apply(Project& project) {
  const int length = ticks_ / 16 * 16;
  if (length < 16 || length > kMaxSong) throw std::out_of_range("a song is 16 to 65536 ticks long");
  oldLength_ = project.song.length;
  oldLanes_ = project.lanes;
  loopBefore_ = project.song.raw.contains("loop") ? project.song.raw["loop"] : Json();
  for (Lane& lane : project.lanes) {
    lane.mutes.erase(std::remove_if(lane.mutes.begin(), lane.mutes.end(), [&](int t) { return t >= length; }), lane.mutes.end());
    lane.offs.erase(std::remove_if(lane.offs.begin(), lane.offs.end(), [&](int t) { return t >= length; }), lane.offs.end());
    lane.placements.erase(std::remove_if(lane.placements.begin(), lane.placements.end(), [&](const Placement& p) { return p.start >= length; }), lane.placements.end());
    for (Placement& p : lane.placements) if (p.start + p.length > length) p.length = length - p.start;
  }
  project.song.length = length;
  if (project.song.raw.contains("loop") && project.song.raw["loop"].is_object()) {   // the loop stays inside the song
    Json& loop = project.song.raw["loop"];
    const int start = loop.value("start", 0), end = std::min(loop.value("end", 0), length);
    if (start >= end) project.song.raw.erase("loop"); else loop["end"] = end;
  }
}

void SetSongLength::revert(Project& project) {
  project.lanes = oldLanes_;
  project.song.length = oldLength_;
  if (loopBefore_.is_null()) project.song.raw.erase("loop"); else project.song.raw["loop"] = loopBefore_;
}

void InsertSilence::apply(Project& project) {
  if (ticks_ < 1) throw std::out_of_range("insert at least one tick");
  if (project.song.length + ticks_ > kMaxSong) throw std::out_of_range("the song would pass 65536 ticks");
  at_ = tick_ / 16 * 16;
  for (Lane& lane : project.lanes) {
    for (Placement& p : lane.placements) p.start = movedBy(p.start, at_, ticks_);
    for (int& t : lane.mutes) t = movedBy(t, at_, ticks_);
    for (int& t : lane.offs) t = movedBy(t, at_, ticks_);
  }
  project.song.length += ticks_;
  loopBefore_ = project.song.raw.contains("loop") ? project.song.raw["loop"] : Json();
  if (project.song.raw.contains("loop") && project.song.raw["loop"].is_object()) {
    Json& loop = project.song.raw["loop"];
    loop["start"] = movedBy(loop.value("start", 0), at_, ticks_);
    const int end = loop.value("end", 0);
    loop["end"] = end > at_ ? end + ticks_ : end;
  }
}

void InsertSilence::revert(Project& project) {
  for (Lane& lane : project.lanes) {
    for (Placement& p : lane.placements) if (p.start >= at_ + ticks_) p.start -= ticks_;
    for (int& t : lane.mutes) if (t >= at_ + ticks_) t -= ticks_;
    for (int& t : lane.offs) if (t >= at_ + ticks_) t -= ticks_;
  }
  project.song.length -= ticks_;
  if (loopBefore_.is_null()) project.song.raw.erase("loop"); else project.song.raw["loop"] = loopBefore_;
}

void SetPatternColor::apply(Project& project) {
  for (Pattern& p : laneNamed(project, lane_).patterns) if (p.id == pattern_) { old_ = p.color; p.color = color_; return; }
  throw std::out_of_range("no pattern " + pattern_);
}

void SetPatternColor::revert(Project& project) {
  for (Pattern& p : laneNamed(project, lane_).patterns) if (p.id == pattern_) p.color = old_;
}

void RenamePattern::apply(Project& project) {
  for (Pattern& p : laneNamed(project, lane_).patterns) if (p.id == pattern_) { old_ = p.name; p.name = name_; return; }
  throw std::out_of_range("no pattern " + pattern_);
}

void RenamePattern::revert(Project& project) {
  for (Pattern& p : laneNamed(project, lane_).patterns) if (p.id == pattern_) p.name = old_;
}

void ClonePattern::apply(Project& project) {
  Lane& lane = laneNamed(project, lane_);
  const Pattern* source = nullptr;
  for (const Pattern& p : lane.patterns) if (p.id == source_) source = &p;
  if (!source) throw std::out_of_range("no pattern " + source_);
  Pattern copy = *source;
  copy.id = copy_;
  copy.name += "-copy";
  std::set<int> used;
  for (const Pattern& p : lane.patterns) used.insert(p.index);
  copy.index = 0;
  while (used.count(copy.index)) copy.index++;
  lane.patterns.push_back(std::move(copy));
}

void ClonePattern::revert(Project& project) {
  std::vector<Pattern>& list = laneNamed(project, lane_).patterns;
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Pattern& p) { return p.id == copy_; }), list.end());
}

void PlacePattern::apply(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    replaced_.clear();
    for (std::size_t i = lane.placements.size(); i-- > 0;) {
      if (lane.placements[i].start != start_) continue;
      replaced_.push_back(lane.placements[i]);
      lane.placements.erase(lane.placements.begin() + static_cast<long>(i));
    }
    Placement placement;
    placement.pattern = pattern_;
    placement.start = start_;
    placement.length = length_;
    lane.placements.push_back(placement);
    return;
  }
  throw std::out_of_range("no lane " + lane_);
}

void PlacePattern::revert(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    for (std::size_t i = lane.placements.size(); i-- > 0;) {
      if (lane.placements[i].pattern == pattern_ && lane.placements[i].start == start_) { lane.placements.erase(lane.placements.begin() + static_cast<long>(i)); break; }
    }
    for (const Placement& p : replaced_) lane.placements.push_back(p);
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

namespace {
std::string nodePart(const std::string& address) { return address.substr(0, address.find('.')); }

// Whether the wires lead from node `start` to node `target` (a node reaches itself).
bool reaches(const Graph& graph, const std::string& start, const std::string& target) {
  std::vector<std::string> todo{start};
  std::set<std::string> seen;
  while (!todo.empty()) {
    const std::string at = todo.back();
    todo.pop_back();
    if (at == target) return true;
    if (!seen.insert(at).second) continue;
    for (const Connection& c : graph.connections) if (nodePart(c.from) == at) todo.push_back(nodePart(c.to));
  }
  return false;
}
}  // namespace

void Connect::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  added_ = false;
  for (const Connection& c : g.connections) if (c.from == from_ && c.to == to_) return;
  if (reaches(g, nodePart(to_), nodePart(from_))) throw std::runtime_error("That would make a loop.");
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

void SetInstrumentSetup::apply(Project& project) {
  if (voices_ < 1 || voices_ > 16) throw std::out_of_range("voices must be 1 to 16");
  Graph& g = graphOf(project, instrument_);
  oldVoices_ = g.voices; oldNeverSleeps_ = g.neverSleeps;
  g.voices = voices_; g.neverSleeps = neverSleeps_;
}

void SetInstrumentSetup::revert(Project& project) {
  Graph& g = graphOf(project, instrument_);
  g.voices = oldVoices_; g.neverSleeps = oldNeverSleeps_;
}

namespace {
int instrumentIndex(const Project& project, const std::string& id) {
  for (std::size_t i = 0; i < project.instruments.size(); i++) if (project.instruments[i].id == id) return static_cast<int>(i);
  return -1;
}

// Why an instrument cannot be renamed or removed: something else still refers to it by name ("" when nothing does).
std::string referencedBy(const Project& project, const std::string& id) {
  for (const Instrument& other : project.instruments) {
    if (other.id == id) continue;
    for (const GraphNode& node : other.graph.nodes) if (node.source == id) return other.id + " listens to it";
    for (const MetaColumn& column : other.metaColumns) if (column.instrument == id) return other.id + " plays it";
  }
  return std::string();
}
}  // namespace

void RenameInstrument::rename(Project& project, const std::string& from, const std::string& to) {
  const int at = instrumentIndex(project, from);
  if (at < 0) throw std::out_of_range("no machine " + from);
  if (from == "master") throw std::runtime_error("The master machine keeps its name.");
  if (to.empty() || instrumentIndex(project, to) >= 0) throw std::runtime_error("A machine called \"" + to + "\" exists already.");
  const std::string why = referencedBy(project, from);
  if (!why.empty()) throw std::runtime_error("Cannot rename: " + why + ".");
  project.instruments[static_cast<std::size_t>(at)].id = to;
  for (Lane& lane : project.lanes) if (lane.instrument == from) lane.instrument = to;
}

void RenameInstrument::apply(Project& project) { rename(project, from_, to_); }
void RenameInstrument::revert(Project& project) { rename(project, to_, from_); }

void DuplicateInstrument::apply(Project& project) {
  const int at = instrumentIndex(project, source_);
  if (at < 0) throw std::out_of_range("no machine " + source_);
  if (source_ == "master") throw std::runtime_error("There is one master machine.");
  if (instrumentIndex(project, copy_) >= 0) throw std::runtime_error("A machine called \"" + copy_ + "\" exists already.");
  Instrument copy = project.instruments[static_cast<std::size_t>(at)];
  copy.id = copy_;
  project.instruments.insert(project.instruments.begin() + at + 1, std::move(copy));
}

void DuplicateInstrument::revert(Project& project) {
  const int at = instrumentIndex(project, copy_);
  if (at >= 0) project.instruments.erase(project.instruments.begin() + at);
}

void RemoveInstrument::apply(Project& project) {
  at_ = instrumentIndex(project, instrument_);
  if (at_ < 0) throw std::out_of_range("no machine " + instrument_);
  if (instrument_ == "master") throw std::runtime_error("The master machine stays.");
  std::string users;
  for (const Lane& lane : project.lanes) if (lane.instrument == instrument_) users += (users.empty() ? "" : ", ") + lane.id;
  if (!users.empty()) throw std::runtime_error("\"" + instrument_ + "\" is played by " + users + "; remove that lane first.");
  const std::string why = referencedBy(project, instrument_);
  if (!why.empty()) throw std::runtime_error("Cannot remove: " + why + ".");
  removed_ = project.instruments[static_cast<std::size_t>(at_)];
  project.instruments.erase(project.instruments.begin() + at_);
}

void RemoveInstrument::revert(Project& project) {
  project.instruments.insert(project.instruments.begin() + at_, removed_);
}

namespace {
Instrument& instrumentNamed(Project& project, const std::string& id) {
  for (Instrument& i : project.instruments) if (i.id == id) return i;
  throw std::out_of_range("no machine " + id);
}
}  // namespace

void RecallPreset::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  const Json* found = nullptr;
  const Json presets = instrument.raw.value("presets", Json::array());
  for (const Json& p : presets) if (p.value("id", -1) == preset_) found = &p;
  if (!found) throw std::out_of_range("no preset " + std::to_string(preset_));
  before_.clear(); addresses_.clear();
  const Json held = found->value("values", Json::object());
  for (const auto& [key, value] : held.items()) {
    if (!value.is_number()) continue;
    const std::size_t dot = key.find('.');
    if (dot == std::string::npos) continue;
    const std::string nodeId = key.substr(0, dot), param = key.substr(dot + 1);
    for (GraphNode& node : instrument.graph.nodes) {
      if (node.id != nodeId) continue;
      if (!node.params.is_object()) node.params = Json::object();
      const bool had = node.params.contains(param);
      before_.push_back({nodeId, param, had, had ? node.params[param] : Json()});
      node.params[param] = value;
      addresses_.push_back(instrument_ + "." + key);
    }
  }
}

void RecallPreset::revert(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  for (const Before& b : before_)
    for (GraphNode& node : instrument.graph.nodes) {
      if (node.id != b.node) continue;
      if (b.had) node.params[b.param] = b.value; else node.params.erase(b.param);
    }
}

void SetPresets::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  if (!instrument.raw.is_object()) instrument.raw = Json::object();
  old_ = instrument.raw.contains("presets") ? instrument.raw["presets"] : Json();
  if (presets_.is_array() && !presets_.empty()) instrument.raw["presets"] = presets_; else instrument.raw.erase("presets");
}

void SetPresets::revert(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  if (old_.is_null()) instrument.raw.erase("presets"); else instrument.raw["presets"] = old_;
}

void SetNoteColumns::apply(Project& project) {
  if (columns_ < 1 || columns_ > 8) throw std::out_of_range("tracks must be 1 to 8");
  Instrument& instrument = instrumentNamed(project, instrument_);
  old_ = instrument.noteColumns;
  instrument.noteColumns = columns_;
}

void SetNoteColumns::revert(Project& project) { instrumentNamed(project, instrument_).noteColumns = old_; }

void MoveInstrument::apply(Project& project) {
  from_ = to_ = -1;
  for (std::size_t i = 0; i < project.instruments.size(); i++) if (project.instruments[i].id == instrument_) from_ = static_cast<int>(i);
  to_ = from_ + direction_;
  if (from_ < 0 || to_ < 0 || to_ >= static_cast<int>(project.instruments.size()) || project.instruments[static_cast<std::size_t>(from_)].id == "master" || project.instruments[static_cast<std::size_t>(to_)].id == "master") { from_ = to_ = -1; return; }
  std::swap(project.instruments[static_cast<std::size_t>(from_)], project.instruments[static_cast<std::size_t>(to_)]);
}

void MoveInstrument::revert(Project& project) {
  if (from_ >= 0) std::swap(project.instruments[static_cast<std::size_t>(from_)], project.instruments[static_cast<std::size_t>(to_)]);
}

void MoveWire::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  replaced_ = duplicate_ = false;
  std::size_t index = g.connections.size();
  for (std::size_t i = 0; i < g.connections.size(); i++) if (g.connections[i].from == from_ && g.connections[i].to == to_) index = i;
  if (index == g.connections.size()) throw std::runtime_error("That wire is gone.");
  const Connection old = g.connections[index];
  g.connections.erase(g.connections.begin() + static_cast<std::ptrdiff_t>(index));
  if (reaches(g, nodePart(newTo_), nodePart(newFrom_))) {
    g.connections.insert(g.connections.begin() + static_cast<std::ptrdiff_t>(index), old);
    throw std::runtime_error("That would make a loop.");
  }
  at_ = index;
  replaced_ = true;
  for (const Connection& c : g.connections) duplicate_ |= c.from == newFrom_ && c.to == newTo_;
  if (!duplicate_) g.connections.insert(g.connections.begin() + static_cast<std::ptrdiff_t>(index), Connection{newFrom_, newTo_});
}

void MoveWire::revert(Project& project) {
  if (!replaced_) return;
  Graph& g = graphOf(project, instrument_);
  if (!duplicate_) {
    for (std::size_t i = 0; i < g.connections.size(); i++) if (g.connections[i].from == newFrom_ && g.connections[i].to == newTo_) { g.connections.erase(g.connections.begin() + static_cast<std::ptrdiff_t>(i)); break; }
  }
  g.connections.insert(g.connections.begin() + static_cast<std::ptrdiff_t>(std::min(at_, g.connections.size())), Connection{from_, to_});
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
  if (!edit.otherLane().empty()) changed_.insert(edit.otherLane());
  if (edit.allLanes()) for (const Lane& l : project_.lanes) changed_.insert(l.id);
  for (const std::string& address : edit.paramAddresses()) changedParams_.insert(address);
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
