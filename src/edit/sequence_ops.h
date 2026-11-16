// The arrangement's edits on one lane, as the web app's seq/edit.js makes them: place, insert, clone, slice, merge, resize, move, remove,
// shift and recolour the lane's patterns, flip its mutes. Each works on a copy of the lane (the caller applies the result as one undo step,
// see SetLaneSequence) and answers whether it changed anything.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "project/project.h"

namespace wax::seq {

struct Context {
  int length = 0;        // the song's length in ticks
  std::string blankRow;  // a row with every cell empty, for the lane's columns
};

inline constexpr int kColorPresets = 16;

// The placement that plays at a tick: of those covering it, the one that starts last. nullptr when none.
const Placement* placementAt(const Lane& lane, int tick);
Pattern* patternOf(Lane& lane, const std::string& id);
const Pattern* patternOf(const Lane& lane, const std::string& id);

// A new blank pattern of `rows` rows placed at `tick`. Refused where one starts, or (without `overlay`) where one plays.
bool insertPattern(Lane& lane, const Context& context, int tick, int rows = 16, bool overlay = false);
// The placement under the tick gets its own copy of the pattern (named "... copy", in another colour).
bool clonePlacement(Lane& lane, int tick);
// Splits the placement under the tick in two there; both halves are new patterns.
bool slicePlacement(Lane& lane, int tick);
// One new pattern of what the lane plays from `from` up to `to`; patterns crossing the ends are cut there.
bool mergeRange(Lane& lane, const Context& context, int from, int to, bool fillEmpty);
// The placement's length changed by `delta` ticks (clipped to the song; growing past the pattern adds empty rows). The new length, 0 if unchanged.
int resizePlacement(Lane& lane, const Context& context, int tick, int delta);
// The placement moved by `delta` ticks, not onto a tick where another starts. Its new start.
std::optional<int> movePlacement(Lane& lane, const Context& context, int tick, int delta);
// Every placement that starts in [from, to) goes (the patterns stay in the list). How many.
int clearRange(Lane& lane, int from, int to);
// The placement under the tick goes; the pattern stays in the lane's list.
bool removePlacement(Lane& lane, int tick);
// Every placement starting at or after `from` moves by `delta`: positive makes room, negative pulls up (no further than the gap). How far they moved.
int shiftPlacements(Lane& lane, const Context& context, int from, int delta);
// The pattern under the tick through the 16 preset colours and back to automatic (delta 1 forwards, -1 back).
bool colourPattern(Lane& lane, int tick, int delta);
// A mute flip at the tick, or the one there taken away.
bool toggleMute(Lane& lane, const Context& context, int tick);
// A note-off mark at the tick, or the one there taken away.
bool toggleOff(Lane& lane, const Context& context, int tick);


// ---- Blocks: copy, cut, paste and duplicate across lanes ----

// A placement taken out of a block: where it was (lane and tick relative to the block's corner) and a copy of its pattern.
struct ClipItem {
  int lane = 0, tick = 0, length = 0;
  std::string sourceLane, sourcePattern;   // where it came from: pasted back there it is the same pattern again
  std::string signature;                   // the columns of the lane it came from: other lanes take the pattern only when theirs match
  Pattern pattern;
};

struct Clip {
  int lanes = 0, ticks = 0;   // the block's size
  std::vector<ClipItem> items;
};

// The placements that start in ticks [from, to) of lanes first..last. `signatures` has one entry per lane (what its columns are).
Clip captureClip(const std::vector<Lane>& lanes, const std::vector<std::string>& signatures, int first, int last, int from, int to);

struct PlaceResult {
  int placed = 0, skipped = 0;        // patterns put down, and those that did not fit (another lane's columns, no lane there, no free index)
  std::vector<int> changed;           // the lanes (by position) that were edited
};

// Puts the clip down with its corner at lane `first`, tick `at`, over whatever starts in that span. A pattern goes back into its own lane as the same
// pattern; into another lane it is copied, when that lane has the same columns. Edits `lanes` (copies the caller then applies).
PlaceResult placeClip(std::vector<Lane>& lanes, const std::vector<std::string>& signatures, const Clip& clip, int first, int at, const Context& context);

}  // namespace wax::seq
