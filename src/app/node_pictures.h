// Pictures beside a node in the graph view that are drawn from its parameters alone (as the web app's per-node views): the echo delays'
// bars against the song's ticks (canyon), the three modulated delays over four seconds (ensemble) and the early/late pulses with the
// mono comb they make (haas). Shown for the picked node, or for nodes told to keep their pictures (Alt+V), like the envelopes.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "app/node_response.h"
#include "app/picture_slot.h"
#include "core/node.h"
#include "project/project.h"

namespace wax::app {

// The song's timing, for pictures that lay time out in ticks.
struct PictureTiming {
  double tickMs = 0;
  int perBeat = 4;
  const Project* project = nullptr;   // for the pictures that show a sample of the song
  std::string instrument;
  int revision = 0;                   // of the document: a sample is read again when it changes
  // The live picture of a node of this instrument (the spectra of the kaleidoscope); empty when nothing is playing.
  std::function<std::shared_ptr<SpectrumProbe>(const std::string& instrument, const std::string& node)> probeOf;
};

bool hasPicture(const std::string& nodeType);

// The nodes whose sound is drawn as Bezier envelopes and heard in a hit (drumsynth2, hat2): their strip is a grid, each cell one drawing or one hit.
bool hasDrawnHitGrid(const std::string& nodeType);
ImU32 drawingColour(const std::string& nodeType, const std::string& drawing);   // the colour a drawing has in the picture and the editor (0: none)
double drawingMs(const GraphNode& node, const NodeType& type, const std::string& drawing);   // how long a drawing takes, from the node's rate settings
int drawnHitCount(const std::string& nodeType);   // how many hits the node plays (a hat: closed and open)
// One hit as the node plays it: its waveform with the ms axis, in the rectangle min to max. `size` is the text size.
void drawHitCell(ImDrawList* dl, ImFont* font, float size, const GraphNode& node, const NodeType& type, ImVec2 min, ImVec2 max, float scale, int hit);

// How big the picture of a node type is, in web pixels (zero when it has none).
PictureSize pictureSize(const std::string& nodeType);

// Draws the picture for `node` and says how far under the tile it reaches (so another picture can go below it, 0 when it went above); what the mouse can take hold of in it is added to `handles` (their param names start with '@').
float drawNodePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, const PictureTiming& timing, ResponseHandles& handles);

// The parameter changes dragging `handle` to the screen point `at` makes.
std::vector<std::pair<std::string, double>> dragPictureHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at);

}  // namespace wax::app
