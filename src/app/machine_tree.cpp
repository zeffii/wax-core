#include "app/machine_tree.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

#include "app/machines.h"
#include "app/theme.h"

namespace wax::app {

namespace {

struct VisibleRow {
  const MachineNode* node;
  const VisibleRow* parent;       // (an index would do; a pointer into the same vector is stable while it is not resized)
  int parentIndex;
  int depth;
  std::string guides;             // in front of the label: 'v' a line passing through, 'e' blank, 't' a branch with more below, 'l' the last branch
};

std::set<std::string>& openGroups() {
  static std::set<std::string> open = {"samplers", "synths", "drums", "tools"};   // the top groups to begin with
  return open;
}
std::string& lastPicked() { static std::string path; return path; }
int cursor = 0;

// The rows that are showing, top to bottom.
void walk(const std::vector<MachineNode>& nodes, int depth, const std::string& passes, int parent, std::vector<VisibleRow>& rows) {
  for (std::size_t i = 0; i < nodes.size(); i++) {
    const MachineNode& node = nodes[i];
    const bool last = i + 1 == nodes.size();
    VisibleRow row{&node, nullptr, parent, depth, depth == 0 ? "" : passes + (last ? 'l' : 't')};
    const int here = static_cast<int>(rows.size());
    rows.push_back(row);
    if (!node.children.empty() && openGroups().count(node.path)) {
      const std::string next = depth == 0 ? passes : passes + (last ? 'e' : 'v');
      walk(node.children, depth + 1, next, here, rows);
    }
  }
}

}  // namespace

void openMachineTree() {
  // open the groups that hold the machine picked last, and start on it
  std::string path = lastPicked();
  for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) openGroups().insert(path.substr(0, slash));
  std::vector<VisibleRow> rows;
  walk(machineTree(), 0, "", -1, rows);
  cursor = 0;
  for (std::size_t i = 0; i < rows.size(); i++) if (rows[i].node->path == lastPicked()) cursor = static_cast<int>(i);
  ImGui::SetNextWindowPos(ImGui::GetMousePos());
  ImGui::OpenPopup("machinetree");
}

std::string drawMachineTree() {
  std::string picked;
  if (!ImGui::BeginPopup("machinetree", ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) return picked;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  std::vector<VisibleRow> rows;
  walk(machineTree(), 0, "", -1, rows);
  const int n = static_cast<int>(rows.size());
  cursor = std::clamp(cursor, 0, std::max(0, n - 1));
  ImGuiIO& io = ImGui::GetIO();

  const auto activate = [&](int i) {
    const VisibleRow& row = rows[static_cast<std::size_t>(i)];
    if (!row.node->children.empty()) {
      if (openGroups().count(row.node->path)) openGroups().erase(row.node->path); else openGroups().insert(row.node->path);
    } else if (!row.node->disabled) {
      lastPicked() = row.node->path;
      picked = row.node->kind;
    }
  };
  if (n) {
    const VisibleRow& at = rows[static_cast<std::size_t>(cursor)];
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) cursor = (cursor + 1) % n;
    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) cursor = (cursor + n - 1) % n;
    else if (ImGui::IsKeyPressed(ImGuiKey_Home)) cursor = 0;
    else if (ImGui::IsKeyPressed(ImGuiKey_End)) cursor = n - 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) activate(cursor);
    else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !at.node->children.empty()) {   // open the group if it is shut, and go to its first machine
      if (!openGroups().count(at.node->path)) openGroups().insert(at.node->path);
      cursor = std::min(cursor + 1, n);   // (the rows are rebuilt next frame)
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
      if (!at.node->children.empty() && openGroups().count(at.node->path)) openGroups().erase(at.node->path);
      else if (at.parentIndex >= 0) cursor = at.parentIndex;
    }
    for (int c = 0; c < io.InputQueueCharacters.Size; c++) {   // a letter: the next row that starts with it
      const int ch = std::tolower(static_cast<int>(io.InputQueueCharacters[c]));
      for (int k = 1; k <= n; k++) {
        const int i = (cursor + k) % n;
        const std::string& name = rows[static_cast<std::size_t>(i)].node->name;
        if (!name.empty() && std::tolower(static_cast<unsigned char>(name[0])) == ch) { cursor = i; break; }
      }
    }
  }

  // the rows: guide lines in front, then the label (a shut group has a + after it)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lineH = ImGui::GetTextLineHeightWithSpacing(), step = 14 * scale;
  for (int i = 0; i < n; i++) {
    const VisibleRow& row = rows[static_cast<std::size_t>(i)];
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = std::max(ImGui::GetContentRegionAvail().x, 190.0f * scale);
    ImGui::PushID(i);
    ImGui::Selectable("##row", i == cursor, 0, ImVec2(w, lineH));
    ImGui::PopID();
    if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) cursor = i;
    if (ImGui::IsItemClicked()) { cursor = i; activate(i); }
    const ImU32 ink = row.node->disabled ? kDimText : i == cursor ? kWarm : kText;
    const ImU32 guide = i == cursor ? kWarm : lineAlpha(160);
    const float midY = at.y + lineH / 2;
    for (std::size_t g = 0; g < row.guides.size(); g++) {
      const float x = at.x + (static_cast<float>(g) + 0.5f) * step;
      const char kind = row.guides[g];
      if (kind == 'v') dl->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + lineH), lineAlpha(160));
      else if (kind == 't' || kind == 'l') {
        dl->AddLine(ImVec2(x, at.y), ImVec2(x, kind == 'l' ? midY : at.y + lineH), g + 1 == row.guides.size() ? guide : lineAlpha(160));
        if (g + 1 == row.guides.size()) dl->AddLine(ImVec2(x, midY), ImVec2(x + step * 0.8f, midY), guide);
      }
    }
    std::string label = row.node->name;
    if (!row.node->children.empty() && !openGroups().count(row.node->path)) label += " +";
    dl->AddText(ImVec2(at.x + static_cast<float>(row.guides.size()) * step + (row.guides.empty() ? 0 : step * 0.2f), at.y + (lineH - ImGui::GetTextLineHeight()) / 2), ink, label.c_str());
    if (i == cursor) ImGui::SetScrollHereY(0.5f);
  }
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "up/down move   left/right fold   enter pick");
  if (!picked.empty()) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return picked;
}

}  // namespace wax::app
