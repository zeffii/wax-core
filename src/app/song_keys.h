// The Song screen's keyboard (the arrangement as the web app's sequence view has it): the cursor and a selected block, the edits at the
// cursor (insert, clone, slice, move, resize, remove, push/pull, mutes, colour), glue, waypoints, copy/cut/paste/duplicate of blocks, and solo and
// silence for the cursor lane. Each edit is one undo step. Called once a frame, while the Song screen is shown with editing on.
#pragma once

#include <string>
#include <vector>

#include "edit/cell_editor.h"
#include "edit/document.h"
#include "edit/sequence_ops.h"
#include "engine/engine.h"
#include "app/sequence_view.h"

namespace wax::app {

// What the keys keep between frames.
struct SongEdit {
  bool selecting = false;        // a block is selected (Shift+arrows): from the anchor to the cursor
  std::string anchorLane;
  int anchorTick = 0;
  bool glueAsk = false;          // G over several lanes, some of them empty there: asking whether they get an empty pattern
  SequenceBlock glueBlock;       // ...and the block it was asked about
  std::vector<Lane> waypoint;    // W: the arrangement as it was; Shift+W goes back to it
  bool hasWaypoint = false;
  seq::Clip clip;                // Ctrl+C / Ctrl+X: the placements of a block with copies of their patterns
};

// What the keys act on, and the places they report to.
struct SongKeysEnv {
  Document& doc;
  CellEditor& editor;
  Engine& engine;
  std::string& selectedLane;     // the lane tab the cursor's lane selects
  std::string& status;           // the status line's message
  std::string& wantLane;         // a lane (and tick) the Pattern screen is to show next frame
  int& wantTick;
  bool& edited;                  // set when the song changed (the engine is brought up to date afterwards)
  bool& editing;                 // the keyboard types into cells (Esc ends it)
};

// The block selected: the anchor's lane and row to the cursor's. `first` is -1 when nothing is selected.
SequenceBlock selectedBlock(const Project& project, const SongEdit& state, const std::string& cursorLane, int cursorTick, int cell);

// One new pattern per lane of the block from what the lane plays there (G). Lanes with nothing there only get one when `fillEmpty`. How many lanes changed.
int glueBlock(Document& doc, Engine& engine, const SequenceBlock& block, bool fillEmpty);

// The empty row of a lane's columns.
std::string blankRowOf(const Engine& engine, const std::string& lane);

void songCursorKeys(SongEdit& state, SongKeysEnv& env);   // movement, selection, the edits at the cursor, glue and waypoints
void songBlockKeys(SongEdit& state, SongKeysEnv& env);    // Ctrl+C, X, V, D
void laneSoloKeys(SongKeysEnv& env);                      // Alt+S and Alt+M, on the Song and Pattern screens

}  // namespace wax::app
