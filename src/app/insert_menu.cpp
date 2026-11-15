#include "app/insert_menu.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "app/theme.h"

namespace wax::app {

namespace {

struct MenuType { const char* type; const char* category; const char* aliases; };
struct MenuGroup { const char* category; const char* group; const char* type; };

#include "app/insert_menu_table.inc"

struct Row {
  std::string label, type;      // a leaf has a type, a branch has children
  std::vector<Row> children;
};

std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

// The hotkey of each row of a level: the index of the first letter or digit of its label that no earlier row has taken (-1: none).
std::vector<int> hotkeys(const std::vector<Row>& rows) {
  std::set<char> taken;
  std::vector<int> keys;
  for (const Row& r : rows) {
    int found = -1;
    const std::string l = lower(r.label);
    for (std::size_t i = 0; i < l.size() && found < 0; i++) {
      if (std::isalnum(static_cast<unsigned char>(l[i])) && !taken.count(l[i])) { found = static_cast<int>(i); taken.insert(l[i]); }
    }
    keys.push_back(found);
  }
  return keys;
}

// The tree: categories, then groups, then nodes. A node not listed in a group shows up after the groups of its category.
std::vector<Row> buildTree(const std::set<std::string>& available) {
  std::map<std::string, std::vector<std::string>> byCategory;
  for (const MenuType& t : kMenuTypes) if (available.count(t.type)) byCategory[t.category].push_back(t.type);
  std::vector<std::string> names;
  for (const char* c : kMenuOrder) if (byCategory.count(c)) names.push_back(c);
  for (const auto& [c, list] : byCategory) if (std::find(names.begin(), names.end(), c) == names.end()) names.push_back(c);
  std::vector<Row> tree;
  for (const std::string& category : names) {
    std::set<std::string> left(byCategory[category].begin(), byCategory[category].end());
    Row top;
    top.label = category;
    std::vector<std::string> groupOrder;
    std::map<std::string, Row> groups;
    for (const MenuGroup& g : kMenuGroups) {
      if (category != g.category || !left.count(g.type)) continue;
      if (!groups.count(g.group)) { groups[g.group].label = g.group; groupOrder.push_back(g.group); }
      groups[g.group].children.push_back(Row{g.type, g.type, {}});
      left.erase(g.type);
    }
    for (const std::string& g : groupOrder) top.children.push_back(groups[g]);
    for (const std::string& t : left) top.children.push_back(Row{t, t, {}});
    tree.push_back(top);
  }
  return tree;
}

struct Found { std::string type, category; };

// The nodes matching the text, best first: names that start with it, then names that contain it, then categories and aliases.
std::vector<Found> search(const std::string& text, const std::set<std::string>& available) {
  const std::string q = lower(text);
  std::vector<Found> found;
  for (const MenuType& t : kMenuTypes) {
    if (!available.count(t.type)) continue;
    const std::string name = lower(t.type);
    if (!q.empty() && name.find(q) == std::string::npos && lower(t.category).find(q) == std::string::npos && lower(t.aliases).find(q) == std::string::npos) continue;
    found.push_back({t.type, t.category});
  }
  const auto rank = [&](const Found& f) { const std::string n = lower(f.type); return q.empty() || n.rfind(q, 0) == 0 ? 0 : n.find(q) != std::string::npos ? 1 : 2; };
  std::stable_sort(found.begin(), found.end(), [&](const Found& a, const Found& b) { return rank(a) != rank(b) ? rank(a) < rank(b) : a.type < b.type; });
  return found;
}

// A row's label with the letter at `underline` underlined, drawn at `at`.
void drawLabel(ImDrawList* dl, ImVec2 at, const std::string& label, int underline, ImU32 colour) {
  dl->AddText(at, colour, label.c_str());
  if (underline < 0) return;
  const float x0 = at.x + ImGui::CalcTextSize(label.substr(0, static_cast<std::size_t>(underline)).c_str()).x;
  const float x1 = x0 + ImGui::CalcTextSize(label.substr(static_cast<std::size_t>(underline), 1).c_str()).x;
  const float y = at.y + ImGui::GetTextLineHeight();
  dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), colour, 1.0f);
}

}  // namespace

void openInsertMenu(InsertMenuState& state, const char* id) {
  state = InsertMenuState();
  ImGui::SetNextWindowPos(ImGui::GetMousePos());
  ImGui::OpenPopup(id);
}

std::string drawInsertMenu(InsertMenuState& state, const char* id, const std::set<std::string>& available) {
  std::string picked;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!ImGui::BeginPopup(id, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) return picked;
  ImGuiIO& io = ImGui::GetIO();
  const std::vector<Row> tree = buildTree(available);
  const float colW = 150 * scale, colH = 260 * scale;

  // the columns that show: the categories, then the children of the chosen row, and so on
  std::vector<const std::vector<Row>*> columns{&tree};
  for (std::size_t k = 0; k < state.sel.size(); k++) {
    const std::vector<Row>& rows = *columns[k];
    if (rows.empty()) break;
    state.sel[k] = std::clamp(state.sel[k], 0, static_cast<int>(rows.size()) - 1);
    const Row& row = rows[static_cast<std::size_t>(state.sel[k])];
    if (row.children.empty()) { state.sel.resize(k + 1); break; }
    if (k + 1 >= state.sel.size()) state.sel.push_back(0);   // a branch's flyout shows at once, with its first row chosen
    columns.push_back(&row.children);
  }
  state.depth = std::clamp(state.depth, 0, static_cast<int>(columns.size()) - 1);

  // the search box: a hint while the tree has the keys
  if (state.searching) {
    if (state.focusSearch) { ImGui::SetKeyboardFocusHere(); state.focusSearch = false; }
    ImGui::SetNextItemWidth(colW * 2);
    if (ImGui::InputText("##insertsearch", state.text, sizeof state.text)) state.active = 0;
  } else {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "Add node: letters pick, / searches");
  }
  ImGui::Dummy(ImVec2(1, 2 * scale));

  if (state.searching) {
    const std::vector<Found> shown = search(state.text, available);
    state.active = std::clamp(state.active, 0, std::max(0, static_cast<int>(shown.size()) - 1));
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !shown.empty()) state.active = (state.active + 1) % static_cast<int>(shown.size());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !shown.empty()) state.active = (state.active + static_cast<int>(shown.size()) - 1) % static_cast<int>(shown.size());
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) state.active = std::min(state.active + 8, std::max(0, static_cast<int>(shown.size()) - 1));
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) state.active = std::max(state.active - 8, 0);
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && !shown.empty()) picked = shown[static_cast<std::size_t>(state.active)].type;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.searching = false; state.text[0] = 0; }
    ImGui::BeginChild("found", ImVec2(colW * 2, colH), false, ImGuiWindowFlags_NoNav);
    if (shown.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "No match");
    const std::string q = lower(state.text);
    for (int i = 0; i < static_cast<int>(shown.size()); i++) {
      const Found& f = shown[static_cast<std::size_t>(i)];
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      ImGui::PushID(i);
      const bool hit = ImGui::Selectable("##row", i == state.active, 0, ImVec2(0, 0));
      ImGui::PopID();
      if (ImGui::IsItemHovered() && io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y > 0) state.active = i;
      if (hit) picked = f.type;
      const std::size_t m = q.empty() ? std::string::npos : lower(f.type).find(q);
      drawLabel(ImGui::GetWindowDrawList(), at, f.type, m == std::string::npos ? -1 : static_cast<int>(m), i == state.active ? kAccent : kText);
      const float cw = ImGui::CalcTextSize(f.category.c_str()).x;
      ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + w - cw, at.y), kDimText, f.category.c_str());
      if (i == state.active) ImGui::SetScrollHereY(0.5f);
    }
    ImGui::EndChild();
  } else {
    // keys: Up/Down move (and wrap), Right or Enter open a branch or insert, Left goes back a level, / or Tab searches, a letter is a hotkey
    const std::vector<Row>& rows = *columns[static_cast<std::size_t>(state.depth)];
    const int n = static_cast<int>(rows.size());
    int& at = state.sel[static_cast<std::size_t>(state.depth)];
    const auto enter = [&](const Row& row) {
      if (!row.children.empty()) { state.sel.resize(static_cast<std::size_t>(state.depth) + 1); state.sel.push_back(0); state.depth++; }
      else picked = row.type;
    };
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n) at = (at + 1) % n;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n) at = (at + n - 1) % n;
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) at = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_End)) at = std::max(0, n - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) at = std::min(at + 8, std::max(0, n - 1));
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) at = std::max(at - 8, 0);
    if (n && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) enter(rows[static_cast<std::size_t>(at)]);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && state.depth > 0) { state.depth--; state.sel.resize(static_cast<std::size_t>(state.depth) + 1); }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) { state.searching = true; state.focusSearch = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && state.depth > 0) { state.depth--; state.sel.resize(static_cast<std::size_t>(state.depth) + 1); }
    for (int c = 0; c < io.InputQueueCharacters.Size; c++) {
      const unsigned ch = io.InputQueueCharacters[c];
      if (ch == '/') { state.searching = true; state.focusSearch = true; state.text[0] = 0; break; }
      const std::vector<int> keys = hotkeys(rows);
      for (int i = 0; i < n; i++) {
        if (keys[static_cast<std::size_t>(i)] >= 0 && static_cast<unsigned>(lower(rows[static_cast<std::size_t>(i)].label)[static_cast<std::size_t>(keys[static_cast<std::size_t>(i)])]) == std::tolower(static_cast<int>(ch))) {
          at = i;
          enter(rows[static_cast<std::size_t>(i)]);
          break;
        }
      }
    }
    // the columns, side by side
    for (std::size_t k = 0; k < columns.size() && picked.empty(); k++) {
      if (k) ImGui::SameLine(0, 4 * scale);
      const std::vector<Row>& level = *columns[k];
      const std::vector<int> keys = hotkeys(level);
      ImGui::PushID(static_cast<int>(k));
      ImGui::BeginChild("col", ImVec2(colW, colH), false, ImGuiWindowFlags_NoNav);
      for (int i = 0; i < static_cast<int>(level.size()); i++) {
        const Row& row = level[static_cast<std::size_t>(i)];
        const bool here = state.sel[std::min(k, state.sel.size() - 1)] == i && k < state.sel.size();
        const ImVec2 at0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(i);
        ImGui::Selectable("##row", here && static_cast<int>(k) == state.depth, 0, ImVec2(0, 0));
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
        if (hovered && !(here && k + 1 == state.sel.size() + 0 && static_cast<int>(k) == state.depth)) {
          if (!(k < state.sel.size() && state.sel[k] == i)) { state.sel.resize(k + 1); state.sel[k] = i; state.depth = static_cast<int>(k); }
        }
        if (ImGui::IsItemClicked()) {
          state.sel.resize(k + 1); state.sel[k] = i; state.depth = static_cast<int>(k);
          if (!row.children.empty()) { state.sel.push_back(0); state.depth = static_cast<int>(k) + 1; } else picked = row.type;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (here && static_cast<int>(k) != state.depth) dl->AddRectFilled(at0, ImVec2(at0.x + w, at0.y + ImGui::GetTextLineHeight()), accentAlpha(40));   // the trail to the open flyout
        drawLabel(dl, at0, row.label, keys[static_cast<std::size_t>(i)], here ? kAccent : kText);
        if (!row.children.empty()) dl->AddText(ImVec2(at0.x + w - ImGui::CalcTextSize(">").x, at0.y), kDimText, ">");
        if (here && static_cast<int>(k) == state.depth) ImGui::SetScrollHereY(0.5f);
      }
      ImGui::EndChild();
      ImGui::PopID();
    }
  }
  if (!picked.empty()) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return picked;
}

}  // namespace wax::app
