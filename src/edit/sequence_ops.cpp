#include "edit/sequence_ops.h"

#include <algorithm>
#include <map>
#include <chrono>
#include <cstdio>
#include <set>

namespace wax::seq {

namespace {

int visibleEnd(const Lane& lane, const Placement& p) {
  const Pattern* pattern = patternOf(lane, p.pattern);
  return p.start + std::min(p.length, pattern ? static_cast<int>(pattern->rows.size()) : 0);
}

int nextIndex(const Lane& lane) {
  std::set<int> used;
  for (const Pattern& p : lane.patterns) used.insert(p.index);
  for (int i = 0; i < 256; i++) if (!used.count(i)) return i;
  return -1;
}

std::string hex2(int n) {
  char text[8];
  std::snprintf(text, sizeof text, "%02X", n);
  return text;
}

// A pattern added to the lane's list (not placed). Returns its id, "" when all 256 indices are taken.
std::string addPattern(Lane& lane, std::vector<std::string> rows, std::string name, const Json& color) {
  const int index = nextIndex(lane);
  if (index < 0) return std::string();
  static int counter = 0;
  Pattern pattern;
  pattern.id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + "-" + std::to_string(counter++);
  pattern.index = index;
  pattern.name = name.empty() ? "pat" + hex2(index) : std::move(name);
  pattern.rows = std::move(rows);
  pattern.color = color;
  lane.patterns.push_back(pattern);
  return pattern.id;
}

Placement* mutablePlacementAt(Lane& lane, int tick) {
  return const_cast<Placement*>(placementAt(lane, tick));
}

bool startsAt(const Lane& lane, int tick) {
  for (const Placement& p : lane.placements) if (p.start == tick) return true;
  return false;
}

}  // namespace

const Placement* placementAt(const Lane& lane, int tick) {
  const Placement* best = nullptr;
  for (const Placement& p : lane.placements) {
    if (p.start <= tick && tick < visibleEnd(lane, p) && (!best || p.start > best->start)) best = &p;
  }
  return best;
}

Pattern* patternOf(Lane& lane, const std::string& id) {
  for (Pattern& p : lane.patterns) if (p.id == id) return &p;
  return nullptr;
}

const Pattern* patternOf(const Lane& lane, const std::string& id) {
  for (const Pattern& p : lane.patterns) if (p.id == id) return &p;
  return nullptr;
}

bool insertPattern(Lane& lane, const Context& context, int tick, int rows, bool overlay) {
  if (tick < 0 || tick >= context.length || startsAt(lane, tick) || (!overlay && placementAt(lane, tick))) return false;
  const std::string id = addPattern(lane, std::vector<std::string>(static_cast<std::size_t>(rows), context.blankRow), "", Json());
  if (id.empty()) return false;
  Placement placement;
  placement.pattern = id;
  placement.start = tick;
  placement.length = std::min(rows, context.length - tick);
  lane.placements.push_back(placement);
  return true;
}

bool clonePlacement(Lane& lane, int tick) {
  Placement* placement = mutablePlacementAt(lane, tick);
  if (!placement) return false;
  const Pattern source = *patternOf(lane, placement->pattern);
  const std::string id = addPattern(lane, source.rows, source.name + " copy", source.color);
  if (id.empty()) return false;
  Pattern* copy = patternOf(lane, id);
  // the copy has to look different at once: an explicit colour moves to the next preset; an automatic one that comes out the same as the original's gets one of its own
  if (source.color.is_number_integer()) copy->color = (source.color.get<int>() + 1) % kColorPresets;
  else if (!source.color.is_null()) copy->color = source.index % kColorPresets;
  else if (copy->index % 6 == source.index % 6) copy->color = (source.index + 8) % kColorPresets;
  placement = mutablePlacementAt(lane, tick);   // (the list of patterns grew; the placements did not)
  placement->pattern = id;
  return true;
}

bool slicePlacement(Lane& lane, int tick) {
  const Placement* found = placementAt(lane, tick);
  if (!found || lane.patterns.size() > 254) return false;
  const Placement old = *found;
  const Pattern source = *patternOf(lane, old.pattern);
  const int n = std::min(old.length, static_cast<int>(source.rows.size())), k = tick - old.start;
  if (k <= 0 || k >= n) return false;
  const std::string first = addPattern(lane, std::vector<std::string>(source.rows.begin(), source.rows.begin() + k), source.name + ".1", source.color);
  const std::string second = addPattern(lane, std::vector<std::string>(source.rows.begin() + k, source.rows.begin() + n), source.name + ".2", source.color);
  if (first.empty() || second.empty()) return false;
  Placement* placement = mutablePlacementAt(lane, tick);
  placement->pattern = first;
  placement->length = k;
  Placement tail;
  tail.pattern = second;
  tail.start = tick;
  tail.length = n - k;
  lane.placements.push_back(tail);
  return true;
}

int clearRange(Lane& lane, int from, int to) {
  const std::size_t before = lane.placements.size();
  lane.placements.erase(std::remove_if(lane.placements.begin(), lane.placements.end(), [&](const Placement& p) { return p.start >= from && p.start < to; }), lane.placements.end());
  return static_cast<int>(before - lane.placements.size());
}

bool mergeRange(Lane& lane, const Context& context, int from, int to, bool fillEmpty) {
  to = std::min(to, context.length);
  if (from < 0 || from >= to) return false;
  std::vector<Placement*> touching;
  for (Placement& p : lane.placements) if (p.start < to && visibleEnd(lane, p) > from) touching.push_back(&p);
  if (touching.empty() && !fillEmpty) return false;
  if (lane.patterns.size() + touching.size() + 1 > 255) return false;
  // what the lane plays in the span: the placements in the order they start (a later one covers an earlier one), empty rows where none plays
  std::vector<std::string> rows(static_cast<std::size_t>(to - from), context.blankRow);
  std::vector<const Placement*> order(touching.begin(), touching.end());
  std::stable_sort(order.begin(), order.end(), [](const Placement* a, const Placement* b) { return a->start < b->start; });
  for (const Placement* p : order) {
    const Pattern* pattern = patternOf(lane, p->pattern);
    for (int t = std::max(from, p->start); t < std::min(to, visibleEnd(lane, *p)); t++) rows[static_cast<std::size_t>(t - from)] = pattern->rows[static_cast<std::size_t>(t - p->start)];
  }
  struct Tail { int start, length; std::vector<std::string> rows; std::string name; Json color; };
  std::vector<Tail> tails;
  std::vector<Placement> keep;
  for (Placement& p : lane.placements) {
    const bool touched = std::find(touching.begin(), touching.end(), &p) != touching.end();
    if (!touched) { keep.push_back(p); continue; }
    const int end = visibleEnd(lane, p);
    const Pattern pattern = *patternOf(lane, p.pattern);
    if (p.start < from) { Placement head = p; head.length = from - p.start; keep.push_back(head); }
    if (end > to) tails.push_back({to, end - to, std::vector<std::string>(pattern.rows.begin() + (to - p.start), pattern.rows.begin() + (end - p.start)), pattern.name + ".2", pattern.color});
  }
  std::set<int> taken;
  for (const Placement& p : keep) taken.insert(p.start);
  lane.placements = keep;
  for (const Tail& t : tails) {
    if (taken.count(t.start)) continue;
    const std::string id = addPattern(lane, t.rows, t.name, t.color);
    if (id.empty()) continue;
    Placement placement;
    placement.pattern = id; placement.start = t.start; placement.length = t.length;
    lane.placements.push_back(placement);
    taken.insert(t.start);
  }
  const std::string made = addPattern(lane, rows, "", Json());
  if (made.empty()) return false;
  Placement placement;
  placement.pattern = made; placement.start = from; placement.length = to - from;
  lane.placements.push_back(placement);
  return true;
}

int resizePlacement(Lane& lane, const Context& context, int tick, int delta) {
  Placement* placement = mutablePlacementAt(lane, tick);
  if (!placement) return 0;
  const int newLength = std::min(std::max(1, placement->length + delta), context.length - placement->start);
  if (newLength == placement->length) return 0;
  Pattern* pattern = patternOf(lane, placement->pattern);
  while (static_cast<int>(pattern->rows.size()) < newLength) pattern->rows.push_back(context.blankRow);
  placement->length = newLength;
  return newLength;
}

std::optional<int> movePlacement(Lane& lane, const Context& context, int tick, int delta) {
  Placement* placement = mutablePlacementAt(lane, tick);
  if (!placement) return std::nullopt;
  const int start = placement->start + delta;
  if (start < 0 || start >= context.length) return std::nullopt;
  for (const Placement& p : lane.placements) if (&p != placement && p.start == start) return std::nullopt;
  placement->start = start;
  return start;
}

bool removePlacement(Lane& lane, int tick) {
  const Placement* placement = placementAt(lane, tick);
  if (!placement) return false;
  lane.placements.erase(lane.placements.begin() + (placement - lane.placements.data()));
  return true;
}

int shiftPlacements(Lane& lane, const Context& context, int from, int delta) {
  int latest = -1, earliest = context.length;
  bool any = false;
  for (const Placement& p : lane.placements) if (p.start >= from) { any = true; latest = std::max(latest, p.start); earliest = std::min(earliest, p.start); }
  if (!any || delta == 0) return 0;
  if (delta > 0) {
    if (latest + delta >= context.length) return 0;
  } else {
    int floor = from;
    for (const Placement& p : lane.placements) if (p.start < from) floor = std::max(floor, visibleEnd(lane, p));
    delta = -std::min(-delta, earliest - floor);
    if (delta == 0) return 0;
  }
  for (Placement& p : lane.placements) if (p.start >= from) p.start += delta;
  return delta;
}

bool colourPattern(Lane& lane, int tick, int delta) {
  const Placement* placement = placementAt(lane, tick);
  if (!placement) return false;
  Pattern* pattern = patternOf(lane, placement->pattern);
  const int at = pattern->color.is_number_integer() ? pattern->color.get<int>() : -1;   // -1: automatic
  const int steps = kColorPresets + 1;
  const int next = (((at + 1 + delta) % steps) + steps) % steps - 1;
  pattern->color = next < 0 ? Json() : Json(next);
  return true;
}

namespace {
bool flip(std::vector<int>& ticks, int tick) {
  const auto at = std::find(ticks.begin(), ticks.end(), tick);
  if (at != ticks.end()) ticks.erase(at);
  else { ticks.push_back(tick); std::sort(ticks.begin(), ticks.end()); }
  return true;
}
}  // namespace

bool toggleMute(Lane& lane, const Context& context, int tick) {
  if (tick < 0 || tick >= context.length) return false;
  return flip(lane.mutes, tick);
}

bool toggleOff(Lane& lane, const Context& context, int tick) {
  if (tick < 0 || tick >= context.length) return false;
  return flip(lane.offs, tick);
}


Clip captureClip(const std::vector<Lane>& lanes, const std::vector<std::string>& signatures, int first, int last, int from, int to) {
  Clip clip;
  clip.lanes = last - first + 1;
  clip.ticks = to - from;
  for (int li = first; li <= last && li < static_cast<int>(lanes.size()); li++) {
    const Lane& lane = lanes[static_cast<std::size_t>(li)];
    for (const Placement& p : lane.placements) {
      if (p.start < from || p.start >= to) continue;
      const Pattern* pattern = patternOf(lane, p.pattern);
      if (!pattern) continue;
      ClipItem item;
      item.lane = li - first; item.tick = p.start - from; item.length = p.length;
      item.sourceLane = lane.id; item.sourcePattern = p.pattern;
      item.signature = signatures[static_cast<std::size_t>(li)];
      item.pattern = *pattern;
      clip.items.push_back(std::move(item));
    }
  }
  return clip;
}

PlaceResult placeClip(std::vector<Lane>& lanes, const std::vector<std::string>& signatures, const Clip& clip, int first, int at, const Context& context) {
  PlaceResult result;
  for (int i = 0; i < clip.lanes; i++) {
    std::vector<const ClipItem*> mine;
    for (const ClipItem& item : clip.items) if (item.lane == i) mine.push_back(&item);
    const int li = first + i;
    if (li >= static_cast<int>(lanes.size())) { result.skipped += static_cast<int>(mine.size()); continue; }
    Lane& lane = lanes[static_cast<std::size_t>(li)];
    Lane work = lane;   // (a lane that does not take everything is left as it was)
    std::map<std::string, std::string> adopted;   // source pattern -> the copy made in this lane
    std::vector<Placement> incoming;
    bool fits = true;
    for (const ClipItem* item : mine) {
      std::string id;
      if (item->sourceLane == work.id && patternOf(work, item->sourcePattern)) id = item->sourcePattern;
      else if (item->signature != signatures[static_cast<std::size_t>(li)]) { fits = false; break; }
      else {
        const std::string key = item->sourceLane + ":" + item->sourcePattern;
        if (!adopted.count(key)) adopted[key] = addPattern(work, item->pattern.rows, item->pattern.name, item->pattern.color);
        id = adopted[key];
        if (id.empty()) continue;
      }
      Placement placement;
      placement.pattern = id; placement.start = at + item->tick; placement.length = item->length;
      if (placement.start < 0 || placement.start >= context.length) continue;
      placement.length = std::min(placement.length, context.length - placement.start);
      incoming.push_back(placement);
    }
    if (!fits) { result.skipped += static_cast<int>(mine.size()); continue; }
    const int to = at + clip.ticks;
    std::vector<Placement> keep;
    for (const Placement& p : work.placements) {
      const bool inside = p.start >= at && p.start < to;
      const bool covered = std::any_of(incoming.begin(), incoming.end(), [&](const Placement& n) { return n.start == p.start; });
      if (!inside && !covered) keep.push_back(p);
    }
    const bool changed = keep.size() != work.placements.size() || !incoming.empty();
    work.placements = keep;
    work.placements.insert(work.placements.end(), incoming.begin(), incoming.end());
    result.placed += static_cast<int>(incoming.size());
    if (changed) { lane = work; result.changed.push_back(li); }
  }
  return result;
}

}  // namespace wax::seq
