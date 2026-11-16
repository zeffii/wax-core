// The Chord node's window: its 256 slots (hex 00 to FF) in pages of 16, each a box for a chord name ("F# 13sus", "Dm7/C", ">G-4 A-5") with the
// notes it makes beside it, a mark on the slot the node's `chord` parameter plays. A name that cannot be read is marked. Kept in the node's
// data.chords ({ "00": "Cmaj7", ... }), changed one slot at a time when its box is left (one undo step).
#pragma once

#include <array>
#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct ChordWindow {
  bool open = false;
  std::string instrument, node;
  int page = 0;                                   // the 16 slots shown: 0 to 15
  std::array<std::array<char, 64>, 16> text{};    // the boxes
  std::array<bool, 16> editing{};                 // which box has the keyboard (its text is not reloaded)
};

void showChords(ChordWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when a slot changed (the caller then runs syncData).
bool drawChordWindow(ChordWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app
