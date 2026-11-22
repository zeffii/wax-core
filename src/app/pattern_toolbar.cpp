#include "app/pattern_toolbar.h"

#include <algorithm>

#include "app/config.h"

namespace wax::app {

namespace {

void iconLanes(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // three columns side by side
  const float w = std::max(1.0f, 1.3f * u);
  for (float x : {2.0f, 6.5f, 11.0f}) dl->AddRect(ImVec2(lo.x + x * u, lo.y + 2 * u), ImVec2(lo.x + (x + 3) * u, lo.y + 14 * u), ink, 0, 0, w);
}

void iconTint(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a block, half filled with colour
  const float w = std::max(1.0f, 1.3f * u);
  dl->AddRect(ImVec2(lo.x + 2 * u, lo.y + 3 * u), ImVec2(lo.x + 14 * u, lo.y + 13 * u), ink, 0, 0, w);
  dl->AddRectFilled(ImVec2(lo.x + 2 * u, lo.y + 3 * u), ImVec2(lo.x + 8 * u, lo.y + 13 * u), ink);
}

void iconHear(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a note: head, stem, flag
  const float w = std::max(1.0f, 1.3f * u);
  dl->AddCircleFilled(ImVec2(lo.x + 5.5f * u, lo.y + 12 * u), 2.4f * u, ink);
  dl->AddLine(ImVec2(lo.x + 7.7f * u, lo.y + 12 * u), ImVec2(lo.x + 7.7f * u, lo.y + 2.5f * u), ink, w);
  dl->AddLine(ImVec2(lo.x + 7.7f * u, lo.y + 2.5f * u), ImVec2(lo.x + 12.5f * u, lo.y + 5.5f * u), ink, w);
}

void iconHideEmpty(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // columns, the middle one thin and dashed (left out)
  const float w = std::max(1.0f, 1.3f * u);
  dl->AddRect(ImVec2(lo.x + 2 * u, lo.y + 2 * u), ImVec2(lo.x + 5 * u, lo.y + 14 * u), ink, 0, 0, w);
  dl->AddRect(ImVec2(lo.x + 11 * u, lo.y + 2 * u), ImVec2(lo.x + 14 * u, lo.y + 14 * u), ink, 0, 0, w);
  for (float y : {2.0f, 6.5f, 11.0f}) dl->AddLine(ImVec2(lo.x + 8 * u, lo.y + y * u), ImVec2(lo.x + 8 * u, lo.y + (y + 3) * u), ink, w);
}

void iconSoloSong(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // a pattern lit inside the song's hatched rows
  const float w = std::max(1.0f, 1.3f * u);
  dl->AddRect(ImVec2(lo.x + 2 * u, lo.y + 2 * u), ImVec2(lo.x + 14 * u, lo.y + 14 * u), ink, 0, 0, w);
  dl->AddRectFilled(ImVec2(lo.x + 4.5f * u, lo.y + 6 * u), ImVec2(lo.x + 11.5f * u, lo.y + 10 * u), ink);
  dl->AddLine(ImVec2(lo.x + 4 * u, lo.y + 4 * u), ImVec2(lo.x + 12 * u, lo.y + 4 * u), ink, w * 0.7f);
  dl->AddLine(ImVec2(lo.x + 4 * u, lo.y + 12 * u), ImVec2(lo.x + 12 * u, lo.y + 12 * u), ink, w * 0.7f);
}

}  // namespace

void iconFollowRow(ImDrawList* dl, ImVec2 lo, float u, ImU32 ink) {   // the playhead line with a chevron pointing the way it moves
  const float w = std::max(1.0f, 1.3f * u);
  dl->AddLine(ImVec2(lo.x + 2 * u, lo.y + 8 * u), ImVec2(lo.x + 14 * u, lo.y + 8 * u), ink, w);
  dl->AddLine(ImVec2(lo.x + 5 * u, lo.y + 3 * u), ImVec2(lo.x + 8 * u, lo.y + 6 * u), ink, w);
  dl->AddLine(ImVec2(lo.x + 11 * u, lo.y + 3 * u), ImVec2(lo.x + 8 * u, lo.y + 6 * u), ink, w);
  dl->AddLine(ImVec2(lo.x + 5 * u, lo.y + 13 * u), ImVec2(lo.x + 8 * u, lo.y + 10 * u), ink, w);
  dl->AddLine(ImVec2(lo.x + 11 * u, lo.y + 13 * u), ImVec2(lo.x + 8 * u, lo.y + 10 * u), ink, w);
}

float drawPatternToolbar(ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, float scale, bool& follow) {
  Settings& s = settings();
  const auto setting = [](const char* name, const char* what, bool& value, auto icon) {
    return ToolbarSwitch{name, what, [&value] { return value; }, [&value] { value = !value; saveSettings(); }, icon};
  };
  const std::vector<ToolbarSwitch> list = {
      setting("All lanes side by side", "every lane's columns in one grid (off: one lane at a time, in tabs)", s.patternAll, iconLanes),
      setting("Tint patterns", "each pattern's rows in the pattern's colour", s.tintPatterns, iconTint),
      setting("Hear notes", "a note typed or moved onto sounds (note preview)", s.notePreview, iconHear),
      setting("Hide empty columns", "leaves out the columns nothing is written in", s.hideEmpty, iconHideEmpty),
      setting("Solo-edit shows the song", "in solo-edit the other patterns stay in view, hatched and locked (off: the pattern alone)", s.soloHatch, iconSoloSong),
      ToolbarSwitch{"Follow the playing row", "the grid scrolls with the playhead while the song plays (F9)", [&follow] { return follow; }, [&follow, &s] { follow = !follow; s.follow = follow; saveSettings(); }, iconFollowRow},
  };
  const ToolbarBox box = toolbarBox(areaMin, areaMax, scale, static_cast<int>(list.size()));
  drawToolbar(dl, box, list, scale, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
  return toolbarHeight(scale) + 2 * kToolbarTop * scale;
}

}  // namespace wax::app
