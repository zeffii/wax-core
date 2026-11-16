#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include "edit/document.h"
#include "project/samples.h"
#include "core/node_registry.h"

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

namespace {
Instrument& instrumentNamed(Project& project, const std::string& id) {
  for (Instrument& i : project.instruments) if (i.id == id) return i;
  throw std::out_of_range("no machine " + id);
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
  lanesBefore_.clear();
  if (cut_) {
    lanesBefore_ = project.lanes;
    static int counter = 0;
    for (Lane& lane : project.lanes) {
      for (std::size_t k = 0, n = lane.placements.size(); k < n; k++) {   // (the halves are added at the end: only the old placements are looked at)
        const Placement across = lane.placements[k];
        const Pattern* whole = nullptr;
        for (const Pattern& p : lane.patterns) if (p.id == across.pattern) whole = &p;
        if (!whole) continue;
        const int shown = std::min(across.length, static_cast<int>(whole->rows.size())), cutAt = at_ - across.start;
        if (cutAt <= 0 || cutAt >= shown) continue;
        std::set<int> used;
        for (const Pattern& p : lane.patterns) used.insert(p.index);
        int firstIndex = 0, secondIndex = 0;
        while (used.count(firstIndex)) firstIndex++;
        used.insert(firstIndex);
        while (used.count(secondIndex)) secondIndex++;
        if (secondIndex > 255) continue;
        const Pattern source = *whole;
        Pattern first = source, second = source;
        first.id = "cut" + std::to_string(counter++); first.index = firstIndex; first.name = source.name + ".1";
        first.rows.assign(source.rows.begin(), source.rows.begin() + cutAt);
        second.id = "cut" + std::to_string(counter++); second.index = secondIndex; second.name = source.name + ".2";
        second.rows.assign(source.rows.begin() + cutAt, source.rows.begin() + shown);
        lane.placements[k].pattern = first.id;
        lane.placements[k].length = cutAt;
        lane.placements.push_back({second.id, at_, shown - cutAt});
        lane.patterns.push_back(std::move(first));
        lane.patterns.push_back(std::move(second));
      }
    }
  }
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
  if (!lanesBefore_.empty()) project.lanes = lanesBefore_;   // (cut: the lanes as they were, halves and all)
  else for (Lane& lane : project.lanes) {
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

SetLaneSequence::SetLaneSequence(const Lane& after, std::string label) : lane_(after.id), label_(std::move(label)) {
  after_ = {after.patterns, after.placements, after.mutes, after.offs};
}

void SetLaneSequence::apply(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    before_ = {lane.patterns, lane.placements, lane.mutes, lane.offs};
    lane.patterns = after_.patterns; lane.placements = after_.placements; lane.mutes = after_.mutes; lane.offs = after_.offs;
    return;
  }
  throw std::out_of_range("no lane " + lane_);
}

void SetLaneSequence::revert(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    lane.patterns = before_.patterns; lane.placements = before_.placements; lane.mutes = before_.mutes; lane.offs = before_.offs;
    return;
  }
}

SetLaneSequences::SetLaneSequences(const std::vector<Lane>& after, std::string label) : label_(std::move(label)) {
  for (const Lane& lane : after) after_.push_back({lane.id, lane.patterns, lane.placements, lane.mutes, lane.offs});
}

void SetLaneSequences::apply(Project& project) {
  before_.clear();
  for (const Sequence& next : after_) {
    for (Lane& lane : project.lanes) {
      if (lane.id != next.lane) continue;
      before_.push_back({lane.id, lane.patterns, lane.placements, lane.mutes, lane.offs});
      lane.patterns = next.patterns; lane.placements = next.placements; lane.mutes = next.mutes; lane.offs = next.offs;
    }
  }
}

void SetLaneSequences::revert(Project& project) {
  for (const Sequence& old : before_) {
    for (Lane& lane : project.lanes) {
      if (lane.id != old.lane) continue;
      lane.patterns = old.patterns; lane.placements = old.placements; lane.mutes = old.mutes; lane.offs = old.offs;
    }
  }
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

namespace {
// Adds a wire unless it is there already; a loop throws.
void addWire(Graph& g, const std::string& from, const std::string& to) {
  for (const Connection& c : g.connections) if (c.from == from && c.to == to) return;
  if (reaches(g, nodePart(to), nodePart(from))) throw std::runtime_error("That would make a loop.");
  g.connections.push_back({from, to});
}
}  // namespace

void InsertOnWire::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  before_ = g.connections;
  try {
    const auto it = std::find_if(g.connections.begin(), g.connections.end(), [&](const Connection& c) { return c.from == from_ && c.to == to_; });
    if (it == g.connections.end()) throw std::runtime_error("That wire is gone.");
    g.connections.erase(it);
    addWire(g, from_, in_);
    addWire(g, out_, to_);
  } catch (...) { g.connections = before_; throw; }
}

void InsertOnWire::revert(Project& project) { graphOf(project, instrument_).connections = before_; }

void PullOut::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  before_ = g.connections;
  try {
    std::vector<std::string> feeds, sinks;
    for (const Connection& c : g.connections) {
      if (c.to == in_) feeds.push_back(c.from);
      if (c.from == out_) sinks.push_back(c.to);
    }
    g.connections.erase(std::remove_if(g.connections.begin(), g.connections.end(), [&](const Connection& c) { return c.to == in_ || c.from == out_; }), g.connections.end());
    for (const std::string& f : feeds) for (const std::string& t : sinks) if (nodePart(f) != nodePart(t)) addWire(g, f, t);
  } catch (...) { g.connections = before_; throw; }
}

void PullOut::revert(Project& project) { graphOf(project, instrument_).connections = before_; }

namespace {
// The node types that play the samples listed for them (the sample choice lives in the machine's "samples" by node id).
bool playsSamples(const std::string& type) {
  static const std::set<std::string> types = {"sampler", "granular", "drumsynth", "drumsynth2", "modchan", "convolver", "specsynth"};
  return types.count(type) > 0;
}

// The port of `names` that stands in for `name` after a node changes type: the same name, else (not for a parameter port) the same position.
const std::string* mappedPort(const std::string& name, const std::vector<std::string>& before, const std::vector<std::string>& after) {
  const auto same = std::find(after.begin(), after.end(), name);
  if (same != after.end()) return &*same;
  if (!name.empty() && name[0] == '@') return nullptr;
  const auto at = std::find(before.begin(), before.end(), name);
  const std::size_t index = static_cast<std::size_t>(at - before.begin());
  return at != before.end() && index < after.size() ? &after[index] : nullptr;
}

// Every input a node has: the type's, then one per parameter given a port.
std::vector<std::string> inputsOf(const GraphNode& node, const NodeType& type) {
  std::vector<std::string> names = type.inputs;
  for (const std::string& p : node.ports) names.push_back("@" + p);
  return names;
}
}  // namespace

void ReplaceNode::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  Graph& g = instrument.graph;
  const auto found = std::find_if(g.nodes.begin(), g.nodes.end(), [&](const GraphNode& n) { return n.id == node_; });
  if (found == g.nodes.end()) throw std::out_of_range("no node " + node_);
  const NodeType* from = builtinNodes().find(found->type);
  const NodeType* to = builtinNodes().find(type_);
  if (!from || !to) throw std::runtime_error("Unknown node type \"" + type_ + "\".");
  if (from->master) throw std::runtime_error("The out node cannot be replaced.");
  if (to->master) throw std::runtime_error("There is already an out node.");
  newId_ = node_;
  if (found->type == type_) return;
  graph_ = g; exposed_ = instrument.exposed; commands_ = instrument.commands; raw_ = instrument.raw;
  const GraphNode old = *found;
  // a node still named for its type ("filter2") is named for the new one
  const std::string stem = old.type;
  const bool named = node_.size() > stem.size() && node_.compare(0, stem.size(), stem) == 0 && std::all_of(node_.begin() + static_cast<std::ptrdiff_t>(stem.size()), node_.end(), [](unsigned char c) { return std::isdigit(c); });
  if (named) {
    for (int i = 1;; i++) {
      newId_ = type_ + std::to_string(i);
      if (std::none_of(g.nodes.begin(), g.nodes.end(), [&](const GraphNode& n) { return n.id == newId_; })) break;
    }
  }
  GraphNode node;
  node.id = newId_; node.type = type_; node.x = old.x; node.y = old.y; node.placed = old.placed;
  node.params = Json::object();
  if (old.params.is_object()) {
    for (const auto& [name, value] : old.params.items()) {
      const auto spec = std::find_if(to->params.begin(), to->params.end(), [&](const ParamSpec& p) { return p.name == name; });
      const auto was = std::find_if(from->params.begin(), from->params.end(), [&](const ParamSpec& p) { return p.name == name; });
      if (spec != to->params.end() && was != from->params.end() && spec->min == was->min && spec->max == was->max) node.params[name] = value;
    }
  }
  for (const std::string& p : old.ports) if (std::any_of(to->params.begin(), to->params.end(), [&](const ParamSpec& s) { return s.name == p; })) node.ports.push_back(p);
  const std::vector<std::string> oldIns = inputsOf(old, *from), newIns = inputsOf(node, *to);
  *std::find_if(g.nodes.begin(), g.nodes.end(), [&](const GraphNode& n) { return n.id == node_; }) = node;
  std::vector<Connection> wires;
  std::set<std::string> seen;
  for (const Connection& c : g.connections) {
    std::string fromNode = c.from.substr(0, c.from.find('.')), fromPort = c.from.substr(c.from.find('.') + 1);
    std::string toNode = c.to.substr(0, c.to.find('.')), toPort = c.to.substr(c.to.find('.') + 1);
    if (fromNode == node_) { const std::string* m = mappedPort(fromPort, from->outputs, to->outputs); if (!m) continue; fromPort = *m; fromNode = newId_; }
    if (toNode == node_) { const std::string* m = mappedPort(toPort, oldIns, newIns); if (!m) continue; toPort = *m; toNode = newId_; }
    const std::string key = fromNode + "." + fromPort + ">" + toNode + "." + toPort;
    if (seen.insert(key).second) wires.push_back({fromNode + "." + fromPort, toNode + "." + toPort});
  }
  g.connections = wires;
  // what pointed at the old id follows it, and what the new type does not have goes
  const auto lacks = [&](const std::string& nodeId, const std::string& param) {
    if (nodeId != newId_) return false;
    return std::none_of(to->params.begin(), to->params.end(), [&](const ParamSpec& p) { return p.name == param; });
  };
  for (Exposed& e : instrument.exposed) if (e.node == node_) e.node = newId_;
  for (Command& c : instrument.commands) if (c.node == node_) c.node = newId_;
  instrument.exposed.erase(std::remove_if(instrument.exposed.begin(), instrument.exposed.end(), [&](const Exposed& e) { return lacks(e.node, e.param); }), instrument.exposed.end());
  instrument.commands.erase(std::remove_if(instrument.commands.begin(), instrument.commands.end(), [&](const Command& c) { return lacks(c.node, c.param); }), instrument.commands.end());
  if (instrument.raw.is_object() && instrument.raw.contains("samples") && instrument.raw["samples"].is_object()) {
    Json& samples = instrument.raw["samples"];
    if (samples.contains(node_)) { Json choice = samples[node_]; samples.erase(node_); if (playsSamples(type_)) samples[newId_] = choice; }
  }
  if (playsSamples(type_) && !sampleId_.empty()) {
    if (!instrument.raw.is_object()) instrument.raw = Json::object();
    if (!instrument.raw.contains("samples") || !instrument.raw["samples"].is_object()) instrument.raw["samples"] = Json::object();
    if (!instrument.raw["samples"].contains(newId_)) instrument.raw["samples"][newId_] = sampleId_;
  }
}

void ReplaceNode::revert(Project& project) {
  if (newId_ == node_ && graph_.nodes.empty()) return;   // (the same type: nothing was changed)
  Instrument& instrument = instrumentNamed(project, instrument_);
  instrument.graph = graph_; instrument.exposed = exposed_; instrument.commands = commands_; instrument.raw = raw_;
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

void RecallPreset::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  const Json* found = nullptr;
  const Json presets = instrument.raw.value("presets", Json::array());
  for (const Json& p : presets) if (p.value("id", -1) == preset_) found = &p;
  if (!found) throw std::out_of_range("no preset " + std::to_string(preset_));
  before_.clear(); beforeData_.clear(); addresses_.clear(); dataNodes_.clear();
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
  const Json drawn = found->value("data", Json::object());   // { node: its data }
  if (drawn.is_object())
    for (const auto& [nodeId, data] : drawn.items())
      for (GraphNode& node : instrument.graph.nodes) {
        if (node.id != nodeId) continue;
        beforeData_.push_back({nodeId, node.data});
        node.data = data;
        dataNodes_.push_back(instrument_ + "\x1f" + nodeId);
      }
}

void RecallPreset::revert(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  for (const Before& b : before_)
    for (GraphNode& node : instrument.graph.nodes) {
      if (node.id != b.node) continue;
      if (b.had) node.params[b.param] = b.value; else node.params.erase(b.param);
    }
  for (const BeforeData& b : beforeData_)
    for (GraphNode& node : instrument.graph.nodes) if (node.id == b.node) node.data = b.data;
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

void AddLane::apply(Project& project) {
  for (const Instrument& i : project.instruments) if (i.id == instrument_) { project.lanes.push_back(blankLane(lane_, instrument_)); return; }
  throw std::out_of_range("no machine " + instrument_);
}

void AddLane::revert(Project& project) {
  for (std::size_t i = project.lanes.size(); i-- > 0;) if (project.lanes[i].id == lane_) { project.lanes.erase(project.lanes.begin() + static_cast<long>(i)); break; }
}

void CloneLaneWithMachine::apply(Project& project) {
  const int at = laneIndex(project, source_);
  if (at < 0) throw std::out_of_range("no lane " + source_);
  if (source_ == "master") throw std::runtime_error("The master lane cannot be cloned.");
  const int machine = instrumentIndex(project, project.lanes[static_cast<std::size_t>(at)].instrument);
  if (machine < 0) throw std::out_of_range("no machine for lane " + source_);
  Instrument copy = project.instruments[static_cast<std::size_t>(machine)];
  copy.id = instrumentCopy_;
  project.instruments.insert(project.instruments.begin() + machine + 1, std::move(copy));
  Lane lane = project.lanes[static_cast<std::size_t>(at)];
  lane.id = laneCopy_;
  lane.instrument = instrumentCopy_;
  lane.silenced = false;
  if (!patterns_) { lane.patterns.clear(); lane.placements.clear(); lane.mutes.clear(); lane.offs.clear(); }
  if (lane.raw.is_object()) { lane.raw["id"] = laneCopy_; if (!curves_) lane.raw.erase("curves"); }
  project.lanes.insert(project.lanes.begin() + at + 1, std::move(lane));
}

void CloneLaneWithMachine::revert(Project& project) {
  const int lane = laneIndex(project, laneCopy_);
  if (lane >= 0) project.lanes.erase(project.lanes.begin() + lane);
  const int machine = instrumentIndex(project, instrumentCopy_);
  if (machine >= 0) project.instruments.erase(project.instruments.begin() + machine);
}

namespace {

// Where an instrument's first shown-parameter column sits in its lanes' patterns: after its note and velocity columns and its triggers
// (the order buildColumns gives, see ownColumns in seq/columns.cpp).
int firstParamCell(const Instrument& instrument) {
  const int pairs = instrument.noteColumns == 0 ? 0 : std::max(1, std::min(8, instrument.noteColumns));
  return pairs * (instrument.noVel ? 1 : 2) + static_cast<int>(instrument.triggers.size());
}

// The cells of every pattern of the lanes that play `instrument`, changed by `edit` row by row (a row is its cells joined by one space).
// `saved` remembers each changed pattern's rows, so the change can be undone. A shown parameter being added or taken away moves the cells
// after it; without this the patterns would be out of step with the columns.
template <typename Edit>
void changeCells(Project& project, const Instrument& instrument, std::map<std::pair<std::string, std::string>, std::vector<std::string>>& saved, Edit edit) {
  if (instrument.isMeta) return;
  for (Lane& lane : project.lanes) {
    if (lane.instrument != instrument.id) continue;
    for (Pattern& pattern : lane.patterns) {
      saved[{lane.id, pattern.id}] = pattern.rows;
      for (std::string& row : pattern.rows) {
        std::vector<std::string> cells;
        std::istringstream words(row);
        for (std::string cell; words >> cell;) cells.push_back(cell);
        if (cells.empty()) continue;
        edit(cells);
        row.clear();
        for (std::size_t i = 0; i < cells.size(); i++) row += (i ? " " : "") + cells[i];
      }
    }
  }
}

}  // namespace

void SetExposed::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  before_ = instrument.exposed;
  const auto found = std::find_if(instrument.exposed.begin(), instrument.exposed.end(), [&](const Exposed& e) { return e.node == node_ && e.param == param_; });
  rowsBefore_.clear();
  if (!on_) {
    if (found == instrument.exposed.end()) return;
    const std::size_t at = static_cast<std::size_t>(firstParamCell(instrument) + (found - instrument.exposed.begin()));
    changeCells(project, instrument, rowsBefore_, [&](std::vector<std::string>& cells) { if (at < cells.size()) cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(at)); });
    instrument.exposed.erase(found);
    return;
  }
  if (found != instrument.exposed.end()) return;
  const GraphNode* node = nullptr;
  for (const GraphNode& n : instrument.graph.nodes) if (n.id == node_) node = &n;
  const NodeType* type = node ? builtinNodes().find(node->type) : nullptr;
  if (!type) throw std::out_of_range("no node " + node_);
  const ParamSpec* spec = nullptr;
  for (const ParamSpec& p : type->params) if (p.name == param_) spec = &p;
  if (!spec) throw std::out_of_range("no parameter " + param_);
  Exposed added;
  added.node = node_;
  added.param = param_;
  added.label = param_.substr(0, 3);
  if (!added.label.empty()) added.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(added.label[0])));
  const auto taken = [&](const std::string& label) { for (const Exposed& e : instrument.exposed) if (e.label == label) return true; return false; };
  while (taken(added.label)) {   // "Cut", "Cu2", "Cu3"...
    const int digit = added.label.size() > 2 && std::isdigit(static_cast<unsigned char>(added.label[2])) ? added.label[2] - '0' + 1 : 2;
    added.label = added.label.substr(0, 2) + std::to_string(digit);
  }
  if (spec->isInt && spec->max - spec->min < 16) added.bits = 4;
  added.log = spec->min > 0 && spec->max / spec->min >= 50;
  const std::size_t at = static_cast<std::size_t>(firstParamCell(instrument) + static_cast<int>(instrument.exposed.size()));
  const std::string blank = added.bits == 4 ? "." : added.bits == 16 ? "...." : "..";
  changeCells(project, instrument, rowsBefore_, [&](std::vector<std::string>& cells) { if (at < cells.size()) cells.insert(cells.begin() + static_cast<std::ptrdiff_t>(at), blank); });
  instrument.exposed.push_back(std::move(added));
}

void SetExposed::revert(Project& project) {
  instrumentNamed(project, instrument_).exposed = before_;
  for (Lane& lane : project.lanes)
    for (Pattern& pattern : lane.patterns) {
      const auto saved = rowsBefore_.find({lane.id, pattern.id});
      if (saved != rowsBefore_.end()) pattern.rows = saved->second;
    }
}

void SetExposedBits::apply(Project& project) {
  for (Exposed& e : instrumentNamed(project, instrument_).exposed) {
    if (e.node != node_ || e.param != param_) continue;
    old_ = e.bits;
    e.bits = bits_;
    return;
  }
  throw std::out_of_range(param_ + " is not shown as a column");
}

void SetExposedBits::revert(Project& project) {
  for (Exposed& e : instrumentNamed(project, instrument_).exposed) if (e.node == node_ && e.param == param_) e.bits = old_;
}

void SetPort::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  for (GraphNode& node : instrument.graph.nodes) {
    if (node.id != node_) continue;
    portsBefore_ = node.ports;
    connectionsBefore_ = instrument.graph.connections;
    const auto at = std::find(node.ports.begin(), node.ports.end(), param_);
    if (on_ && at == node.ports.end()) node.ports.push_back(param_);
    if (!on_ && at != node.ports.end()) {
      node.ports.erase(at);
      const std::string target = node_ + ".@" + param_;
      auto& wires = instrument.graph.connections;
      wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Connection& c) { return c.to == target; }), wires.end());
    }
    return;
  }
  throw std::out_of_range("no node " + node_);
}

void SetPort::revert(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  for (GraphNode& node : instrument.graph.nodes) if (node.id == node_) node.ports = portsBefore_;
  instrument.graph.connections = connectionsBefore_;
}

void SetKeepView::apply(Project& project) {
  before_.clear();
  for (GraphNode& node : instrumentNamed(project, instrument_).graph.nodes) {
    if (std::find(nodes_.begin(), nodes_.end(), node.id) == nodes_.end()) continue;
    before_.emplace_back(node.id, node.keepView);
    node.keepView = on_;
  }
}

void SetKeepView::revert(Project& project) {
  for (GraphNode& node : instrumentNamed(project, instrument_).graph.nodes)
    for (const auto& [id, was] : before_) if (id == node.id) node.keepView = was;
}

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

void MoveNodes::apply(Project& project) {
  const bool first = before_.empty();
  for (std::size_t i = 0; i < places_.size(); i++) {
    GraphNode& node = findGraphNode(project, instrument_, places_[i].node);
    if (first) before_.push_back({node.x, node.y, node.placed});
    node.x = places_[i].x; node.y = places_[i].y; node.placed = true;
  }
}

void MoveNodes::revert(Project& project) {
  for (std::size_t i = 0; i < places_.size() && i < before_.size(); i++) {
    GraphNode& node = findGraphNode(project, instrument_, places_[i].node);
    node.x = before_[i].x; node.y = before_[i].y; node.placed = before_[i].placed;
  }
}

bool MoveNodes::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const MoveNodes*>(&next);
  if (!other || other->instrument_ != instrument_ || other->places_.size() != places_.size()) return false;
  for (std::size_t i = 0; i < places_.size(); i++) if (places_[i].node != other->places_[i].node) return false;
  places_ = other->places_;
  return true;
}

void DuplicateNodes::apply(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  Graph& g = instrument.graph;
  before_ = g.connections; rawBefore_ = instrument.raw;
  std::map<std::string, std::string> renamed;
  for (const auto& [from, to] : copies_) {
    const auto found = std::find_if(g.nodes.begin(), g.nodes.end(), [&](const GraphNode& n) { return n.id == from; });
    if (found == g.nodes.end() || found->type == "out") continue;
    GraphNode copy = *found;
    copy.id = to; copy.x += dx_; copy.y += dy_; copy.placed = true;
    g.nodes.push_back(copy);
    renamed[from] = to;
    if (instrument.raw.is_object() && instrument.raw.contains("samples") && instrument.raw["samples"].is_object() && instrument.raw["samples"].contains(from)) {
      const Json choice = instrument.raw["samples"][from];
      instrument.raw["samples"][to] = choice;
    }
  }
  std::vector<Connection> wires;
  for (const Connection& c : g.connections) {
    const auto a = renamed.find(nodePart(c.from)), b = renamed.find(nodePart(c.to));
    if (a != renamed.end() && b != renamed.end()) wires.push_back({a->second + c.from.substr(c.from.find('.')), b->second + c.to.substr(c.to.find('.'))});
  }
  g.connections.insert(g.connections.end(), wires.begin(), wires.end());
}

void DuplicateNodes::revert(Project& project) {
  Instrument& instrument = instrumentNamed(project, instrument_);
  for (const auto& copy : copies_) {
    const auto at = std::find_if(instrument.graph.nodes.begin(), instrument.graph.nodes.end(), [&](const GraphNode& n) { return n.id == copy.second; });
    if (at != instrument.graph.nodes.end()) instrument.graph.nodes.erase(at);
  }
  instrument.graph.connections = before_;
  instrument.raw = rawBefore_;
}

void JoinAround::apply(Project& project) {
  Graph& g = graphOf(project, instrument_);
  before_ = g.connections;
  try {
    std::vector<std::string> feeds, sinks;
    for (const Connection& c : g.connections) {
      if (std::find(inputs_.begin(), inputs_.end(), c.to) != inputs_.end()) feeds.push_back(c.from);
      if (c.from == output_) sinks.push_back(c.to);
    }
    for (const std::string& f : feeds) for (const std::string& t : sinks) if (nodePart(f) != nodePart(t)) addWire(g, f, t);
  } catch (...) { g.connections = before_; throw; }
}

void JoinAround::revert(Project& project) { graphOf(project, instrument_).connections = before_; }

SetBank::SetBank(std::string instrument, std::string node, std::vector<int> bytes, std::string what)
    : instrument_(std::move(instrument)), node_(std::move(node)), what_(std::move(what)), bytes_(std::move(bytes)) {}

void SetBank::apply(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (!captured_) { beforeData_ = node.data; captured_ = true; }
  if (!node.data.is_object()) node.data = Json::object();
  node.data["bank"] = bytes_;
}

void SetNodeData::apply(Project& project) {
  GraphNode& node = findGraphNode(project, instrument_, node_);
  if (!captured_) { beforeData_ = node.data; captured_ = true; }
  if (!node.data.is_object()) node.data = Json::object();
  const auto put = [&](const std::string& key, const Json& value) { if (value.is_null()) node.data.erase(key); else node.data[key] = value; };
  put(key_, value_);
  for (const auto& [key, value] : also_) put(key, value);
}

void SetNodeData::revert(Project& project) { findGraphNode(project, instrument_, node_).data = beforeData_; }

bool SetNodeData::absorb(const Edit& next) {
  const auto* other = dynamic_cast<const SetNodeData*>(&next);
  if (!other || other->dataNode() != dataNode() || other->key_ != key_ || other->also_.size() != also_.size()) return false;
  for (std::size_t i = 0; i < also_.size(); i++) if (other->also_[i].first != also_[i].first) return false;
  value_ = other->value_;
  also_ = other->also_;
  return true;
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

void AddSample::apply(Project& project) {
  if (!project.samples.is_object()) project.samples = Json::object();
  project.samples[id_] = sample_;
}

void AddSample::revert(Project& project) {
  if (project.samples.is_object()) project.samples.erase(id_);
}

std::vector<std::string> instrumentsPlaying(const Project& project, const std::string& sampleId) {
  std::vector<std::string> out;
  for (const Instrument& instrument : project.instruments) {
    bool plays = false;
    for (const GraphNode& node : instrument.graph.nodes) {
      const std::vector<std::string> ids = sampleSlots(instrument, node.id);
      plays |= std::find(ids.begin(), ids.end(), sampleId) != ids.end();
    }
    if (plays) out.push_back(instrument.id);
  }
  return out;
}

void RemoveSample::apply(Project& project) {
  before_ = project.samples;
  project.samples.erase(id_);
}

void RemoveSample::revert(Project& project) { project.samples = before_; }

void RenameSample::apply(Project& project) {
  before_ = project.samples;
  instrumentSamples_.clear();
  Json renamed = Json::object();
  for (const auto& [key, value] : project.samples.items()) renamed[key == from_ ? to_ : key] = value;
  project.samples = renamed;
  for (Instrument& instrument : project.instruments) {
    if (!instrument.raw.is_object() || !instrument.raw.contains("samples") || !instrument.raw["samples"].is_object()) continue;
    instrumentSamples_.emplace_back(instrument.id, instrument.raw["samples"]);
    for (auto& [node, ref] : instrument.raw["samples"].items()) {
      if (ref.is_string() && ref.get<std::string>() == from_) ref = to_;
      else if (ref.is_array()) for (Json& slot : ref) if (slot.is_string() && slot.get<std::string>() == from_) slot = to_;
    }
  }
}

void RenameSample::revert(Project& project) {
  project.samples = before_;
  for (const auto& [id, samples] : instrumentSamples_)
    for (Instrument& instrument : project.instruments) if (instrument.id == id) instrument.raw["samples"] = samples;
}

void SetSample::apply(Project& project) {
  if (!project.samples.is_object()) project.samples = Json::object();
  old_ = project.samples.contains(id_) ? project.samples[id_] : Json();
  project.samples[id_] = sample_;
}

void SetSample::revert(Project& project) {
  if (!project.samples.is_object()) return;
  if (old_.is_null()) project.samples.erase(id_); else project.samples[id_] = old_;
}

bool SetSample::absorb(const Edit& next) {
  const SetSample* more = dynamic_cast<const SetSample*>(&next);
  if (!fold_ || !more || more->id_ != id_ || more->label_ != label_) return false;
  sample_ = more->sample_;
  return true;
}

void SetCurves::apply(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    if (!lane.raw.is_object()) lane.raw = Json::object();
    old_ = lane.raw.contains("curves") ? lane.raw["curves"] : Json();
    if (curves_.is_array() && !curves_.empty()) lane.raw["curves"] = curves_; else lane.raw.erase("curves");
  }
}

void SetCurves::revert(Project& project) {
  for (Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    if (!old_.is_null()) lane.raw["curves"] = old_; else if (lane.raw.is_object()) lane.raw.erase("curves");
  }
}

bool SetCurves::absorb(const Edit& next) {
  const SetCurves* other = dynamic_cast<const SetCurves*>(&next);
  if (!other || other->lane_ != lane_ || other->label_ != label_) return false;
  curves_ = other->curves_;   // the drag carries on: the old curves stay what undo goes back to
  return true;
}

void FollowedEdit::apply(Project& project) {
  primary_->apply(project);
  for (std::unique_ptr<Edit>& follower : followers_) follower->apply(project);
}

void FollowedEdit::revert(Project& project) {
  for (auto it = followers_.rbegin(); it != followers_.rend(); ++it) (*it)->revert(project);
  primary_->revert(project);
}

void Document::touched(const Edit& edit) {
  if (const FollowedEdit* followed = dynamic_cast<const FollowedEdit*>(&edit)) {   // the edit and what followed it
    touched(followed->primary());
    for (const std::unique_ptr<Edit>& follower : followed->followers()) touched(*follower);
    return;
  }
  revision_++;
  if (!edit.lane().empty()) changed_.insert(edit.lane());
  if (!edit.otherLane().empty()) changed_.insert(edit.otherLane());
  if (edit.allLanes()) for (const Lane& l : project_.lanes) changed_.insert(l.id);
  for (const std::string& address : edit.paramAddresses()) changedParams_.insert(address);
  for (const std::string& node : edit.dataNodes()) changedData_.insert(node);
  if (edit.songLoop()) loopChanged_ = true;
  if (!edit.graphInstrument().empty()) changedGraphs_.insert(edit.graphInstrument());
  if (!edit.sampleId().empty()) changedSamples_.insert(edit.sampleId());
}

std::vector<std::unique_ptr<Edit>> Document::pinFollowers(const std::map<std::string, std::vector<CurveSpan>>& before) {
  std::vector<std::unique_ptr<Edit>> out;
  for (Lane& lane : project_.lanes) {
    const auto was = before.find(lane.id);
    if (was == before.end()) continue;
    const std::vector<CurveSpan> now = curveSpans(lane);
    const auto same = [](const CurveSpan& a, const CurveSpan& b) { return a.pattern == b.pattern && a.start == b.start && a.length == b.length; };
    if (now.size() == was->second.size() && std::equal(now.begin(), now.end(), was->second.begin(), same)) continue;
    const Json curves = lane.raw.value("curves", Json::array());
    Json updated = Json::array();
    bool changed = false;
    for (const Json& json : curves) {
      LaneCurve curve = readLaneCurve(json);
      const Json unchanged = writeLaneCurve(curve);
      followPins(curve, was->second, now);
      const Json moved = writeLaneCurve(curve);
      if (moved != unchanged) changed = true;
      updated.push_back(moved);
    }
    if (!changed) continue;
    std::unique_ptr<Edit> follower = std::make_unique<SetCurves>(lane.id, updated, "Pinned anchors follow");
    follower->apply(project_);
    touched(*follower);
    out.push_back(std::move(follower));
  }
  return out;
}

void Document::apply(std::unique_ptr<Edit> edit, bool foldIntoPrevious) {
  redo_.clear();
  std::map<std::string, std::vector<CurveSpan>> pinned;   // the lanes whose curves have pinned anchors, with their placements as they were
  if (!dynamic_cast<const SetCurves*>(edit.get()))
    for (const Lane& lane : project_.lanes) {
      const Json curves = lane.raw.is_object() ? lane.raw.value("curves", Json::array()) : Json::array();
      for (const Json& curve : curves)
        for (const Json& point : curve.value("pts", Json::array()))
          if (point.contains("pin")) { pinned[lane.id] = curveSpans(lane); break; }
    }
  edit->apply(project_);
  touched(*edit);
  std::vector<std::unique_ptr<Edit>> followers = pinned.empty() ? std::vector<std::unique_ptr<Edit>>() : pinFollowers(pinned);
  if (!followers.empty()) {   // the pinned anchors moved with their patterns: one step with the edit
    undo_.push_back(std::make_unique<FollowedEdit>(std::move(edit), std::move(followers)));
    return;
  }
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

std::set<std::string> Document::takeChangedSamples() {
  std::set<std::string> out;
  out.swap(changedSamples_);
  return out;
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
