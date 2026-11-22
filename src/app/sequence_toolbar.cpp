#include "app/sequence_toolbar.h"

#include <algorithm>

#include "app/config.h"
#include "app/pattern_toolbar.h"

namespace wax::app {

namespace {

float stroke(float u) { return std::max(1.0f, 1.3f * u); }

void iconNames(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a tile with a number and a name line
  dl->AddRect(ImVec2(lo.x + 2 * u, lo.y + 2 * u), ImVec2(lo.x + 14 * u, lo.y + 14 * u), ink, 0, 0, stroke(u));
  dl->AddRectFilled(ImVec2(lo.x + 4 * u, lo.y + 4.5f * u), ImVec2(lo.x + 6.5f * u, lo.y + 6.5f * u), ink);
  dl->AddLine(ImVec2(lo.x + 4 * u, lo.y + 10 * u), ImVec2(lo.x + 12 * u, lo.y + 10 * u), ink, stroke(u));
  dl->AddLine(ImVec2(lo.x + 4 * u, lo.y + 12 * u), ImVec2(lo.x + 9 * u, lo.y + 12 * u), ink, stroke(u));
}

void iconMinimap(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a tile with small note ticks
  dl->AddRect(ImVec2(lo.x + 2 * u, lo.y + 2 * u), ImVec2(lo.x + 14 * u, lo.y + 14 * u), ink, 0, 0, stroke(u));
  for (const ImVec2 at : {ImVec2(4, 4), ImVec2(8, 6.5f), ImVec2(6, 9), ImVec2(10, 11)}) dl->AddRectFilled(ImVec2(lo.x + at.x * u, lo.y + at.y * u), ImVec2(lo.x + (at.x + 2.2f) * u, lo.y + (at.y + 1.3f) * u), ink);
}

void iconLengths(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a note with a thin line running down from it
  dl->AddRectFilled(ImVec2(lo.x + 5 * u, lo.y + 2.5f * u), ImVec2(lo.x + 11 * u, lo.y + 4.5f * u), ink);
  dl->AddLine(ImVec2(lo.x + 8 * u, lo.y + 4.5f * u), ImVec2(lo.x + 8 * u, lo.y + 13 * u), ink, stroke(u) * 0.8f);
}

void iconCaps(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a line ending in a wide cap
  dl->AddLine(ImVec2(lo.x + 8 * u, lo.y + 2.5f * u), ImVec2(lo.x + 8 * u, lo.y + 11 * u), ink, stroke(u) * 0.8f);
  dl->AddRectFilled(ImVec2(lo.x + 4 * u, lo.y + 11 * u), ImVec2(lo.x + 12 * u, lo.y + 13.5f * u), ink);
}

void iconParams(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // narrow columns of different heights
  for (float x : {3.5f, 6.5f, 9.5f, 12.5f}) dl->AddRectFilled(ImVec2(lo.x + (x - 0.8f) * u, lo.y + (x < 8 ? 6.0f : 3.0f) * u), ImVec2(lo.x + (x + 0.8f) * u, lo.y + 13 * u), ink);
}

void iconOverview(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a tall thin strip with a box on the part in view
  dl->AddRect(ImVec2(lo.x + 5.5f * u, lo.y + 1.5f * u), ImVec2(lo.x + 10.5f * u, lo.y + 14.5f * u), ink, 0, 0, stroke(u));
  dl->AddRectFilled(ImVec2(lo.x + 5.5f * u, lo.y + 5 * u), ImVec2(lo.x + 10.5f * u, lo.y + 9 * u), ink);
}

void iconLoop(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a closed rounded path with an arrowhead
  const float w = stroke(u);
  dl->AddRect(ImVec2(lo.x + 2 * u, lo.y + 4 * u), ImVec2(lo.x + 14 * u, lo.y + 12 * u), ink, 3 * u, 0, w);
  dl->AddTriangleFilled(ImVec2(lo.x + 7 * u, lo.y + 1.5f * u), ImVec2(lo.x + 7 * u, lo.y + 6.5f * u), ImVec2(lo.x + 10.5f * u, lo.y + 4 * u), ink);
}

}  // namespace

float drawSequenceToolbar(ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, float scale, bool& follow, bool loopOn, bool loopSet, const std::function<void()>& toggleLoop) {
  Settings& s = settings();
  const auto setting = [](const char* name, const char* what, bool& value, auto icon) {
    return ToolbarSwitch{name, what, [&value] { return value; }, [&value] { value = !value; saveSettings(); }, icon};
  };
  const std::vector<ToolbarSwitch> list = {
      setting("Pattern names", "the pattern's name beside its number in the tiles (off: just the number; H). The status bar always gives the name", s.seqNames, iconNames),
      setting("Pattern minimaps", "each tile shows a small picture of its pattern (V)", s.minimapOn, iconMinimap),
      setting("Minimaps: note lengths", "a thin line from each note to the next event", s.minimapLengths, iconLengths),
      setting("Minimaps: note off and cut", "cyan for ===, magenta for ^^^", s.minimapCaps, iconCaps),
      setting("Minimaps: parameters", "a narrow column each at the right edge, brighter the higher the value", s.minimapParams, iconParams),
      setting("Song overview", "a strip at the right showing the whole song, the loop and the part in view", s.seqOverview, iconOverview),
      ToolbarSwitch{"Loop", loopSet ? "the song's loop on or off (Ctrl+L); its points are kept" : "no loop set: Ctrl+B and Ctrl+E set one", [loopOn] { return loopOn; }, toggleLoop, iconLoop},
      ToolbarSwitch{"Follow the playing row", "the sheet scrolls with the playhead while the song plays (F9)", [&follow] { return follow; }, [&follow, &s] { follow = !follow; s.follow = follow; saveSettings(); }, iconFollowRow},
  };
  const ToolbarBox box = toolbarBox(areaMin, areaMax, scale, static_cast<int>(list.size()));
  drawToolbar(dl, box, list, scale, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
  return toolbarHeight(scale) + 2 * kToolbarTop * scale;
}

}  // namespace wax::app
