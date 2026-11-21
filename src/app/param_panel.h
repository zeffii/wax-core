// The parameter panel, as the web app's parameter view, here a window that floats over the graph: a title with the node's name, and one
// row per parameter: its name, then a bar with the value written inside it. Drag a bar to set the value (double-click resets, the wheel
// nudges), right-click for a menu to type a value, reset, show the parameter as a pattern column (and its width) or give it a port.
// Rows of a parameter shown as a column carry its label and the cell's hex; a dot marks a parameter with a port.
#pragma once

#include <map>
#include <set>
#include <string>

#include "edit/document.h"
#include "engine/engine.h"
#include "imgui.h"

namespace wax::app {

// One window of exposed sliders: where it is and what its rows needed in height.
struct ExposedWindow {
  bool placed = false;   // put at its first place once; after that it stays where the user left it
  float content = 0;     // the rows' height last frame
  int cursor = 0;        // the row the keys act on
  ImVec2 min{0, 0}, max{0, 0};   // as drawn last
};

struct ParamPanelState {
  std::string instrument;   // the instrument the node choice belongs to
  std::string node;         // the node picked ("" = the first one with parameters)
  std::string drawNode;     // set when the user asked to draw this node's envelopes (the caller opens the window and clears it)
  std::string shapeNode;    // set when the user asked to draw this node's wave or curve
  std::string tableNode;    // set when the user asked for the wavetable window of this node
  std::string chordNode;    // set when the user asked for the chord names of this node
  std::string gateNode;     // set when the user asked for the step levels of this node (gate, arpeggiator)
  std::string dxNode;       // set when the user asked for the DX loader of this node
  std::string vectorNode;
  std::string genexprNode;   // set when the user asked to edit this genexpr node's code   // set when the user asked for the diamond / wave sequences of this node (compass, waveseq, vecpath, vecmix)
  bool dragging = false;    // a drag is in progress: its steps are one undo
  bool placed = false;      // the window has been put at the graph's corner once; after that it stays where the user left it
  float areaRight = 0;      // the right edge of the area last frame: when it moves (the node bank opening) the window moves with it
  int paramCursor = 0;      // the row the keys act on
  float paramContent = 0;   // what the panel's rows needed in height last frame (it is as high as that)
  ExposedWindow dock;       // the exposed dock in the graph screen (min/max: as drawn last, zero size when none, for the graph to keep clear of)
  ExposedWindow follow;     // the floating window of the cursor lane's machine on the Song and Pattern screens
  bool followOpen = false;  // ... and whether it is shown
  std::map<std::string, ExposedWindow> pinned;   // a window kept open for each pinned machine (its id)
  std::set<std::string> pinsSeen;                // the pins as keepPins last saw them (to tell what the user changed)
  bool open = false;        // the window is shown: a double-click on a node opens it, its close button hides it
};

// Whether a node type has a window of its own to edit something (drawings, steps, code, a wave...): the buttons of that window are what
// drawEditorButtons draws (one text button for each, setting the matching field of `state` for the caller to open it).
bool hasEditorWindow(const std::string& nodeType);
void drawEditorButtons(ParamPanelState& state, const GraphNode& picked);

// One parameter's name and bar at the cursor (the row of the panel), `total` wide with the name `nameW` of it. Returns true when the value changed.
bool drawParamBar(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrumentId, const GraphNode& node, const ParamSpec& spec, float total, float nameW);

// Draws the window floating over the graph area (screen rectangle `areaMin`..`areaMax`: where it first appears, at the top right).
// Changes go through `doc` as SetParam edits (and SetExposed, SetExposedBits, SetPort); the caller then runs syncParams, syncLanes and
// syncGraphs. Returns true when a parameter changed.
bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrument, ImVec2 areaMin, ImVec2 areaMax);

// The exposed-parameter dock, at the left of the graph: a row for each column the machine's patterns have (named by the column's label), the
// same bars as the parameter panel. Shown only when something is exposed. Returns true when a parameter changed.
bool drawExposedDock(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrument, ImVec2 areaMin, ImVec2 areaMax);

// The floating windows over the Song and Pattern screens: one for `followInstrument` (the cursor lane's machine; its X hides it, `followOpen`
// shows it) with a pin that keeps a window for that machine open, and a window for each pinned machine (its X, or unpin, puts it away).
// Returns true when a parameter changed.
// Keeps the pins across sessions and songs (call once a frame, before drawing): what the user pinned or unpinned since the last call is
// saved in the settings, and the remembered machines that this song has are pinned, those it lacks put away (but not forgotten).
void keepPins(ParamPanelState& state, const Project& project);

bool drawFloatPanels(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& followInstrument, ImVec2 areaMin, ImVec2 areaMax);

}  // namespace wax::app
