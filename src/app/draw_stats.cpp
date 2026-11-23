#include "app/draw_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "app/theme.h"
#include "imgui.h"

namespace wax::app {
namespace {

bool gOn = false;
std::chrono::steady_clock::time_point gStarted;
int gCommands = 0, gVertices = 0, gIndices = 0;
double gMillis = 0, gWorst = 0;   // the last frame's time; the worst of the last second or so
int gFramesSinceWorst = 0;
char gLine[160] = "";

}  // namespace

void drawStatsSet(bool on) { gOn = on; gWorst = 0; gFramesSinceWorst = 0; }
bool drawStatsOn() { return gOn; }

void drawStatsFrameStart() {
  if (gOn) gStarted = std::chrono::steady_clock::now();
}

void drawStatsFrameEnd(const ImDrawData* data) {
  if (!gOn || !data) return;
  gMillis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gStarted).count();
  if (++gFramesSinceWorst > 60) { gFramesSinceWorst = 0; gWorst = 0; }
  gWorst = std::max(gWorst, gMillis);
  gCommands = 0;
  for (int i = 0; i < data->CmdListsCount; i++) gCommands += data->CmdLists[i]->CmdBuffer.Size;
  gVertices = data->TotalVtxCount;
  gIndices = data->TotalIdxCount;
  std::snprintf(gLine, sizeof gLine, "draw commands %d, vertices %d, indices %d, cpu %.1f ms (worst %.1f)", gCommands, gVertices, gIndices, gMillis, gWorst);
}

void drawStatsOverlay() {
  if (!gOn || !gLine[0]) return;
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::CalcTextSize(gLine);
  const ImVec2 corner = ImGui::GetMainViewport()->Pos;
  const float right = corner.x + ImGui::GetMainViewport()->Size.x - 8;
  const ImVec2 at(right - size.x, corner.y + 4);
  draw->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(right + 4, at.y + size.y + 2), IM_COL32(0, 0, 0, 190));
  draw->AddText(at, kText, gLine);
}

const char* drawStatsLine() { return gLine; }

}  // namespace wax::app
