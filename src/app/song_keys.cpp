#include "app/song_keys.h"

#include <algorithm>
#include <functional>
#include <optional>

#include <imgui.h>

#include "app/config.h"
#include "edit/sync.h"
#include "seq/cells.h"
#include "seq/loop.h"

namespace wax::app {

SequenceBlock selectedBlock(const Project& project, const SongEdit& state, const std::string& cursorLane, int cursorTick, int cell) {
  SequenceBlock block;
  if (!state.selecting) return block;
  int a = -1, b = -1, index = 0;
  for (const Lane& lane : project.lanes) {
    if (lane.id == state.anchorLane) a = index;
    if (lane.id == cursorLane) b = index;
    index++;
  }
  if (a < 0 || b < 0) return block;
  block.first = std::min(a, b); block.last = std::max(a, b);
  block.from = std::min(state.anchorTick, cursorTick) / cell * cell;
  block.to = (std::max(state.anchorTick, cursorTick) / cell + 1) * cell;
  return block;
}

std::string blankRowOf(const Engine& engine, const std::string& lane) {
  std::string blank;
  const std::vector<Column>& columns = engine.laneColumns(lane);
  for (std::size_t i = 0; i < columns.size(); i++) blank += (i ? " " : "") + tokenBlank(columns[i].token);
  return blank;
}

int glueBlock(Document& doc, Engine& engine, const SequenceBlock& block, bool fillEmpty) {
  std::vector<Lane> changed;
  for (int i = block.first; i <= block.last && i < static_cast<int>(doc.project().lanes.size()); i++) {
    Lane copy = doc.project().lanes[static_cast<std::size_t>(i)];
    seq::Context context;
    context.length = doc.project().song.length;
    context.blankRow = blankRowOf(engine, copy.id);
    if (seq::mergeRange(copy, context, block.from, block.to, fillEmpty)) changed.push_back(copy);
  }
  if (changed.empty()) return 0;
  doc.apply(std::make_unique<SetLaneSequences>(changed, "merge to pattern"));
  return static_cast<int>(changed.size());
}

void songCursorKeys(SongEdit& state, SongKeysEnv& env) {
  Document& doc = env.doc;
  CellEditor& editor = env.editor;
  Engine& engine = env.engine;
  std::string& status = env.status;
  std::string& wantLane = env.wantLane;
  std::string& selectedLane = env.selectedLane;
  int& wantTick = env.wantTick;
  bool& edited = env.edited;
  bool& editing = env.editing;
  const Song& song = doc.project().song;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl || io.KeyAlt) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { editing = false; state.selecting = false; return; }
  if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_H, false)) { settings().seqNames = !settings().seqNames; saveSettings(); status = settings().seqNames ? "Pattern names shown." : "Pattern names hidden."; }
  {   // the sequence's cursor: a row is `cell` ticks, the lanes run across
    const int cell = sequenceRowTicks(song), rows = std::max(1, (song.length + cell - 1) / cell);
    const auto toRow = [&](int row) { editor.moveTo(std::clamp(row, 0, rows - 1) * cell, editor.cell()); };
    const int row = editor.tick() / cell;
    // Shift with a movement key grows a block from where the cursor was; Delete clears it, G glues it; any other key drops it
    const ImGuiKey movers[] = {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_PageUp, ImGuiKey_PageDown, ImGuiKey_Home, ImGuiKey_End, ImGuiKey_LeftArrow, ImGuiKey_RightArrow};
    bool extending = false;
    if (io.KeyShift) for (ImGuiKey k : movers) extending |= ImGui::IsKeyPressed(k);
    if (extending && !state.selecting) { state.selecting = true; state.anchorLane = editor.lane(); state.anchorTick = editor.tick(); }
    if (!extending && state.selecting) {
      for (int k = ImGuiKey_Tab; k < ImGuiKey_GamepadStart; k++) {   // (the keyboard's keys)
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (key == ImGuiKey_Delete || key == ImGuiKey_Backspace || key == ImGuiKey_G || (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper)) continue;
        if (ImGui::IsKeyPressed(key, false)) { state.selecting = false; break; }
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) toRow(row - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) toRow(row + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) toRow(row - 16);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) toRow(row + 16);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) toRow(0);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) toRow(rows - 1);
    int lanes = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) lanes = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) lanes = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) lanes = io.KeyShift ? -1 : 1;
    if (lanes != 0 && !doc.project().lanes.empty()) {
      const std::vector<Lane>& all = doc.project().lanes;
      const auto at = std::find_if(all.begin(), all.end(), [&](const Lane& l) { return l.id == editor.lane(); });
      const std::ptrdiff_t next = std::clamp<std::ptrdiff_t>((at - all.begin()) + lanes, 0, static_cast<std::ptrdiff_t>(all.size()) - 1);
      const std::string lane = all[static_cast<std::size_t>(next)].id;
      if (lane != editor.lane()) {
        const int tick = editor.tick();
        selectedLane = lane;
        editor.setLane(lane, engine.laneColumns(lane));
        editor.moveTo(tick, 0);
        wantLane = lane; wantTick = tick;
      }
    }
    // the arrangement's edits at the cursor, each one undo step (the web's sequence keys); the letters are commands here, not notes
    const std::string laneId = editor.lane();
    const Lane* current = nullptr;
    for (const Lane& l : doc.project().lanes) if (l.id == laneId) current = &l;
    if (current) {
      const int tick = editor.tick(), rowStart = tick / cell * cell;
      seq::Context context;
      context.length = song.length;
      context.blankRow = editor.blankRowText();
      const auto change = [&](const char* label, const char* done, const std::function<bool(Lane&)>& edit) {
        Lane copy = *current;
        if (!edit(copy)) { status = std::string("Cannot ") + label + " here."; return false; }
        doc.apply(std::make_unique<SetLaneSequence>(copy, label));
        edited = true;
        status = done;
        return true;
      };
      const auto pressed = [&](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
      if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {   // L: the loop on or off; Shift+L: a loop round the pattern under the cursor
        Json loop;
        std::string message;
        bool changed = true;
        if (io.KeyShift) {
          const Placement* at = seq::placementAt(*current, rowStart);
          const Pattern* pattern = at ? seq::patternOf(*current, at->pattern) : nullptr;
          loop = loopAround(song, at ? at->start : 0, at && pattern ? std::min(at->length, static_cast<int>(pattern->rows.size())) : -1, message, changed);
        } else {
          loop = loopToggled(song);
          changed = !loop.is_null();
          message = changed ? (loop.value("off", false) ? "Loop off (its points are kept)." : "Loop on.") : "No loop to switch: Ctrl+B and Ctrl+E set one.";
        }
        if (changed) { doc.apply(std::make_unique<SetLoop>(std::move(loop), message)); syncLoop(doc, engine); }
        status = message;
      }
      const SequenceBlock block = selectedBlock(doc.project(), state, laneId, tick, cell);
      const bool clearing = block.first >= 0 && (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace));
      if (clearing) {   // the patterns that start inside the block go (they stay in the lanes' lists)
        std::vector<Lane> changed;
        int removed = 0;
        for (int i = block.first; i <= block.last; i++) {
          Lane copy = doc.project().lanes[static_cast<std::size_t>(i)];
          const int n = seq::clearRange(copy, block.from, block.to);
          if (n) { removed += n; changed.push_back(copy); }
        }
        if (removed) { doc.apply(std::make_unique<SetLaneSequences>(changed, "clear block")); edited = true; }
        status = removed ? "Removed " + std::to_string(removed) + (removed == 1 ? " pattern." : " patterns.") : "Nothing there.";
        state.selecting = false;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !io.KeyShift) {   // glue the block into one new pattern per lane
        if (block.first < 0) status = "Select a block first (Shift+arrows).";
        else {
          bool someEmpty = false;
          for (int i = block.first; i <= block.last; i++) {
            const Lane& l = doc.project().lanes[static_cast<std::size_t>(i)];
            bool plays = false;
            for (const Placement& p : l.placements) {
              const Pattern* pat = seq::patternOf(l, p.pattern);
              if (p.start < block.to && pat && p.start + std::min(p.length, static_cast<int>(pat->rows.size())) > block.from) plays = true;
            }
            someEmpty |= !plays;
          }
          if (block.last > block.first && someEmpty) { state.glueAsk = true; state.glueBlock = block; }   // (asked below: do the empty lanes get a pattern too?)
          else {
            const int n = glueBlock(doc, engine, block, true);
            edited |= n > 0;
            status = n == 0 ? "Nothing to merge there." : n == 1 ? "Merged into one new pattern." : "Merged into " + std::to_string(n) + " new patterns, one per lane.";
            state.selecting = false;
          }
        }
      }
      if (!clearing && (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace))) {
        if (seq::placementAt(*current, tick)) change("remove the pattern", "Removed. The pattern stays in the list.", [&](Lane& l) { return seq::removePlacement(l, tick); });
        else change("pull up", "Pulled the patterns below up.", [&](Lane& l) { return seq::shiftPlacements(l, context, rowStart, -16) != 0; });   // an empty spot: close the gap, 16 ticks at most
      }
      if (pressed(ImGuiKey_Insert)) change("push down", "Pushed the patterns from here down 16 ticks.", [&](Lane& l) { return seq::shiftPlacements(l, context, rowStart, 16) != 0; });
      if (!io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_I, false)) change("insert a pattern", "Pattern inserted.", [&](Lane& l) { return seq::insertPattern(l, context, rowStart, 16, true); });
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) change("clone", "Cloned: this placement now has its own copy.", [&](Lane& l) { return seq::clonePlacement(l, tick); });
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) change("slice", "Sliced.", [&](Lane& l) { return seq::slicePlacement(l, tick); });
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) change("mute", "Mute marker flipped here.", [&](Lane& l) { return seq::toggleMute(l, context, rowStart); });
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) change("mark a note off", "Note-off marker flipped here.", [&](Lane& l) { return seq::toggleOff(l, context, rowStart); });
      }
      if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {   // W: remember the arrangement as it is; Shift+W: go back to it (an undoable step)
        if (!io.KeyShift) { state.waypoint = doc.project().lanes; state.hasWaypoint = true; status = "Waypoint set."; }
        else if (!state.hasWaypoint) status = "No waypoint set.";
        else { doc.apply(std::make_unique<SetLaneSequences>(state.waypoint, "restore waypoint")); edited = true; status = "Restored the waypoint."; }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_K, false)) change("change the colour", "Pattern colour changed.", [&](Lane& l) { return seq::colourPattern(l, tick, io.KeyShift ? -1 : 1); });   // Shift+K steps back
      if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd)) change("extend", "Extended.", [&](Lane& l) { return seq::resizePlacement(l, context, tick, 16) != 0; });
      if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) change("shrink", "Shrunk.", [&](Lane& l) { return seq::resizePlacement(l, context, tick, -16) != 0; });
      const struct { ImGuiKey key; int by; } moves[] = {{ImGuiKey_Period, 1}, {ImGuiKey_Comma, -1}};   // . and , move the pattern a row; Shift: sixteen rows
      for (const auto& m : moves) {
        if (!pressed(m.key)) continue;
        const int delta = m.by * cell * (io.KeyShift ? 16 : 1);
        int movedTo = -1;
        if (change("move", "Moved.", [&](Lane& l) { const std::optional<int> at = seq::movePlacement(l, context, tick, delta); if (at) movedTo = *at; return at.has_value(); })) editor.moveTo(movedTo, editor.cell());
      }
  }
    }
}

void songBlockKeys(SongEdit& state, SongKeysEnv& env) {
  Document& doc = env.doc;
  CellEditor& editor = env.editor;
  Engine& engine = env.engine;
  std::string& status = env.status;
  std::string& wantLane = env.wantLane;
  std::string& selectedLane = env.selectedLane;
  int& wantTick = env.wantTick;
  bool& edited = env.edited;
  const Song& song = doc.project().song;
  const ImGuiIO& io = ImGui::GetIO();
  if (!io.KeyCtrl || io.KeyAlt || io.KeyShift || editor.lane().empty()) return;
    const std::vector<Lane>& all = doc.project().lanes;
    const int cell = sequenceRowTicks(song), rowStart = editor.tick() / cell * cell;
    std::vector<std::string> signatures;   // lanes with the same columns can take each other's patterns
    for (const Lane& l : all) {
      std::string sig;
      for (const Column& c : engine.laneColumns(l.id)) sig += std::to_string(static_cast<int>(c.token)) + ":" + c.label + "|";
      signatures.push_back(sig);
    }
    int laneAt = 0;
    for (std::size_t i = 0; i < all.size(); i++) if (all[i].id == editor.lane()) laneAt = static_cast<int>(i);
    // the block: the selection, else the pattern under the cursor (as many rows tall as it is)
    SequenceBlock block = selectedBlock(doc.project(), state, editor.lane(), editor.tick(), cell);
    if (block.first < 0) {
      block.first = block.last = laneAt; block.from = block.to = rowStart;
      const Lane& here = all[static_cast<std::size_t>(laneAt)];
      if (const Placement* p = seq::placementAt(here, editor.tick())) {
        const Pattern* pat = seq::patternOf(here, p->pattern);
        const int rows = std::min(p->length, pat ? static_cast<int>(pat->rows.size()) : p->length);
        block.from = p->start / cell * cell; block.to = (p->start + rows - 1) / cell * cell;
      }
      block.to += cell;   // (to is exclusive)
    }
    seq::Context context;
    context.length = song.length;
    const auto count = [](int n) { return std::to_string(n) + (n == 1 ? " pattern" : " patterns"); };
    const auto putCursor = [&](int lane, int tick) {   // the cursor moves; the lane tab follows
      const std::string id = all[static_cast<std::size_t>(lane)].id;
      selectedLane = id;
      editor.setLane(id, engine.laneColumns(id));
      editor.moveTo(tick, 0);
      wantLane = id; wantTick = tick;
    };
    // puts the clip down at a lane and tick and selects what was put down
    const auto place = [&](const seq::Clip& clip, int lane, int tick, const char* label) {
      std::vector<Lane> work = all;
      const seq::PlaceResult r = seq::placeClip(work, signatures, clip, lane, tick, context);
      if (!r.changed.empty()) {
        std::vector<Lane> changed;
        for (int i : r.changed) changed.push_back(work[static_cast<std::size_t>(i)]);
        doc.apply(std::make_unique<SetLaneSequences>(changed, label));
        edited = true;
      }
      const int lastLane = std::min(static_cast<int>(all.size()) - 1, lane + clip.lanes - 1);
      const int lastTick = std::min(song.length - 1, tick + clip.ticks - cell) / cell * cell;
      state.selecting = lastLane > lane || lastTick > tick;
      state.anchorLane = all[static_cast<std::size_t>(lane)].id; state.anchorTick = tick;
      putCursor(lastLane, lastTick);
      return r;
    };
    const auto capture = [&]() { return seq::captureClip(all, signatures, block.first, block.last, block.from, block.to); };
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)) {
      const bool cut = ImGui::IsKeyPressed(ImGuiKey_X, false);
      seq::Clip clip = capture();
      if (clip.items.empty()) status = "Nothing to copy here.";
      else {
        state.clip = clip;
        if (cut) {
          std::vector<Lane> changed;
          for (int i = block.first; i <= block.last; i++) { Lane copy = all[static_cast<std::size_t>(i)]; if (seq::clearRange(copy, block.from, block.to)) changed.push_back(copy); }
          doc.apply(std::make_unique<SetLaneSequences>(changed, "cut")); edited = true;
          state.selecting = false;
        }
        status = std::string(cut ? "Cut " : "Copied ") + count(static_cast<int>(clip.items.size())) + ".";
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
      const seq::Clip& clip = state.clip;
      if (clip.items.empty()) status = "Nothing copied.";
      else if (block.from >= song.length) status = "No room here.";
      else {
        const int at = state.selecting ? block.from : rowStart, lane = state.selecting ? block.first : laneAt;
        const seq::PlaceResult r = place(clip, lane, at, "paste");
        status = r.placed ? "Pasted " + count(r.placed) + (r.skipped ? ", " + std::to_string(r.skipped) + " did not fit." : ".") : r.skipped ? "Those lanes have different columns." : "Nothing changed.";
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {   // the block is copied to the rows right below it; the selection moves down with the copy, so again copies it once more
      const seq::Clip clip = capture();
      const int down = clip.ticks, at = block.from + down;
      if (clip.items.empty()) status = "Nothing to copy here.";
      else if (at >= song.length) status = "No room below.";
      else {
        const seq::PlaceResult r = place(clip, block.first, at, "copy down");
        status = r.placed ? "Copied " + count(static_cast<int>(clip.items.size())) + " down." : "Nothing changed.";
      }
    }
}

void laneSoloKeys(SongKeysEnv& env) {
  Document& doc = env.doc;
  CellEditor& editor = env.editor;
  std::string& status = env.status;
  bool& edited = env.edited;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyAlt && !io.KeyCtrl && !io.KeyShift && !editor.lane().empty()) {
    // Alt+S solos the lane under the cursor (again to bring everyone back); Alt+M silences it (the pair of the mixer's S and M)
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      const std::optional<bool> on = doc.toggleSolo(editor.lane());
      bool any = false;
      for (const Lane& l : doc.project().lanes) any |= l.solo;
      status = !on ? "The master lane cannot be soloed." : *on ? editor.lane() + " solo." : any ? editor.lane() + " unsoloed." : "Solo off: every lane plays.";
      edited = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
      bool silenced = false;
      for (const Lane& l : doc.project().lanes) if (l.id == editor.lane()) silenced = l.silenced;
      doc.apply(std::make_unique<SetSilenced>(editor.lane(), !silenced));
      status = silenced ? editor.lane() + " is back on." : editor.lane() + " muted.";
      edited = true;
    }
  }
}

}  // namespace wax::app
