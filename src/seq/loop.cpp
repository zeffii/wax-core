#include "seq/loop.h"

#include <algorithm>

namespace wax {

LoopRange loopOf(const Song& song) {
  LoopRange r;
  r.start = 0;
  r.end = song.length;
  if (!song.raw.is_object() || !song.raw.contains("loop") || !song.raw["loop"].is_object()) return r;
  const Json& loop = song.raw["loop"];
  r.start = std::clamp(static_cast<int>(loop.value("start", 0.0)), 0, std::max(0, song.length - 1));
  r.end = std::clamp(static_cast<int>(loop.value("end", static_cast<double>(song.length))), r.start + 1, std::max(1, song.length));
  r.on = !loop.value("off", false);
  return r;
}

Json loopWithPoint(const Song& song, const std::string& which, int tick, std::string& message) {
  const bool has = song.raw.is_object() && song.raw.contains("loop") && song.raw["loop"].is_object();
  const LoopRange range = loopOf(song);
  int s = range.start, e = range.end;
  if (which == "start") {
    tick = std::clamp(tick, 0, std::max(0, song.length - 1));
    if (has && tick == range.start) { s = 0; message = "Loop start cleared."; }
    else { s = tick; if (s >= e) e = song.length; message = "Loop starts at " + std::to_string(s) + "."; }
  } else {
    tick = std::clamp(tick, 1, std::max(1, song.length));
    if (has && tick == range.end) { e = song.length; message = "Loop end cleared."; }
    else { e = tick; if (e <= s) s = 0; message = "Loop ends at " + std::to_string(e) + "."; }
  }
  if (s == 0 && e == song.length) return Json();
  return Json{{"start", s}, {"end", e}};
}

Json loopToggled(const Song& song) {
  if (!song.raw.is_object() || !song.raw.contains("loop") || !song.raw["loop"].is_object()) return Json();
  Json loop = song.raw["loop"];
  if (loop.value("off", false)) loop.erase("off"); else loop["off"] = true;
  return loop;
}

}  // namespace wax
