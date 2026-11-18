// The node strip: a narrow band over the bottom of the graph (as the device view of a DAW) that shows, side by side, the UI of every node
// of the instrument that has something to draw or edit, in the order of the graph: the node's parameter bars, its picture (response,
// echoes, room, ...), its envelope picture, one editable Bezier canvas for each of its drawings, and the buttons of its editor windows.
// The web app shows the same UIs below the graph; here they sit on top of it. A thin grip on the strip's top edge sets its height; pulled
// all the way down it hides (the grip stays; pull it up to bring the strip back). Clicking a node's name picks the node.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "imgui.h"

#include "app/envelope_editor.h"
#include "app/node_response.h"
#include "app/param_panel.h"
#include "core/probe.h"
#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct NodeStripState {
  ParamPanelState bars;                          // (only its drag flag is used: the bars of the strip fold a drag into one undo step)
  std::map<std::string, EnvelopeView> views;     // one per drawing ("node/drawing"): the chosen anchor, snap and so on
  ResponseHandles response;                      // what the mouse can take hold of in the pictures drawn this frame
  int responseDrag = -1;                         // the handle being dragged (its index in `response.list`), or -1
  bool responseFresh = false;                    // the drag has not changed anything yet (its first change starts the undo step)
  float dragHeight = 0;                          // the height the grip is being pulled to (web pixels)
};

struct NodeStripResult {
  bool paramsChanged = false;   // a parameter was set: the caller runs syncParams
  bool dataChanged = false;     // a drawing was changed: the caller runs syncData
};

// Whether a node of this type has a UI the strip shows.
bool hasStripUi(const std::string& nodeType);

// Draws the strip over the bottom of the graph area `areaMin`..`areaMax`. `panel` is where the editor buttons leave their requests and which
// node is picked; `probeOf` gives the live pictures their audio.
NodeStripResult drawNodeStrip(NodeStripState& state, ParamPanelState& panel, Document& doc, const Engine& engine, const std::string& instrumentId, ImVec2 areaMin, ImVec2 areaMax,
                              const std::function<std::shared_ptr<SpectrumProbe>(const std::string& instrument, const std::string& node)>& probeOf);

}  // namespace wax::app
