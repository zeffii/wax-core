#include "app/genexpr_window.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "app/theme.h"
#include "app/widgets.h"
#include "nodes/genexpr_check.h"

namespace wax::app {
namespace {

constexpr std::size_t kMaxCode = 65536;
constexpr double kFollowSeconds = 0.28;   // the sound follows the text this long after typing stops

struct GenExprTemplate { const char* name; bool instrument; const char* code; };
#include "app/genexpr_templates.inc"

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

std::string codeOf(const GraphNode& node) {
  if (node.data.is_object() && node.data.contains("code") && node.data["code"].is_string()) return node.data["code"].get<std::string>();
  return genexprDefaultCode();
}

void load(GenExprWindow& w, const std::string& code) {
  w.buffer.assign(std::max(kMaxCode, code.size() + 4096), 0);
  std::memcpy(w.buffer.data(), code.data(), code.size());
}

// Tab types two spaces (as in the web app); a click on a mistake selects it.
int editCallback(ImGuiInputTextCallbackData* data) {
  GenExprWindow& w = *static_cast<GenExprWindow*>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
    if (data->EventChar != '\t') return 0;
    data->InsertChars(data->CursorPos, "  ");
    return 1;
  }
  if (w.selectFrom >= 0) {
    data->SelectionStart = w.selectFrom;
    data->SelectionEnd = data->CursorPos = w.selectTo;
    w.selectFrom = -1;
  }
  return 0;
}

// The offset in `text` of a 1-based line and column.
int offsetOf(const std::string& text, int line, int col) {
  std::size_t at = 0;
  for (int i = 1; i < line; i++) {
    const std::size_t next = text.find('\n', at);
    if (next == std::string::npos) break;
    at = next + 1;
  }
  return static_cast<int>(std::min(text.size(), at + static_cast<std::size_t>(std::max(0, col - 1))));
}

void drawTemplates(GenExprWindow& w, const std::string& text, std::string& next) {
  ImGui::SetNextItemWidth(220 * ImGui::GetStyle().FontScaleDpi);
  if (!ImGui::BeginCombo("##templates", "Templates...")) return;
  for (const bool instruments : {true, false}) {
    ImGui::TextDisabled("%s", instruments ? "Instruments (in front of Voices)" : "Effects");
    for (const GenExprTemplate& t : kGenExprTemplates) {
      if (t.instrument != instruments) continue;
      if (ImGui::Selectable(t.name)) { w.before = text; next = t.code; }
    }
  }
  if (!w.before.empty()) {
    ImGui::Separator();
    if (ImGui::Selectable("the code I had before")) { const std::string now = text; next = w.before; w.before = now; }
  }
  ImGui::EndCombo();
}

}  // namespace

void showGenExpr(GenExprWindow& window, const std::string& instrument, const std::string& node) {
  window = GenExprWindow{};
  window.open = true;
  window.instrument = instrument;
  window.node = node;
}

bool drawGenExprWindow(GenExprWindow& w, Document& doc, const Engine&) {
  if (!w.open) return false;
  const GraphNode* node = findNode(doc.project(), w.instrument, w.node);
  if (!node || node->type != "genexpr") { w.open = false; return false; }
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(640 * scale, 520 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(260 * scale, 90 * scale), ImGuiCond_FirstUseEver);
  bool changed = false;
  const std::string title = "Code  " + w.instrument + "." + w.node + "###genexpr";
  if (ImGui::Begin(title.c_str(), &w.open, ImGuiWindowFlags_NoSavedSettings)) {
    // The node's code, when it changed under us (an undo) and the box is not being typed in.
    const std::string held = codeOf(*node);
    if (w.buffer.empty() || (held != w.applied && w.changedAt == 0)) { load(w, held); w.applied = held; }

    const std::string text = w.buffer.data();
    std::string chosen;
    drawTemplates(w, text, chosen);
    if (!chosen.empty()) { load(w, chosen); w.changedAt = 1e-9; w.typing = false; }   // (a template is one undo step of its own)

    const float boxHeight = std::max(120.0f * scale, ImGui::GetContentRegionAvail().y - 150 * scale);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.95f);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackAlways;
    const bool edited = ImGui::InputTextMultiline("##code", w.buffer.data(), w.buffer.size(), ImVec2(-1, boxHeight), flags, editCallback, &w);
    ImGui::PopFont();
    if (edited || !chosen.empty()) w.changedAt = ImGui::GetTime() - (chosen.empty() ? 0 : kFollowSeconds);   // (a template applies at once)

    // what the compiler says
    const std::string now = w.buffer.data();
    const GenExprReport report = checkGenExpr(now);
    if (report.ok) {
      char line[96];
      std::snprintf(line, sizeof line, "compiled%s%s", report.params.empty() ? "" : " - ", report.params.empty() ? "" : (std::to_string(report.params.size()) + (report.params.size() > 1 ? " knobs" : " knob")).c_str());
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "%s", line);
    } else {
      ImGui::TextColored(ImVec4(0.93f, 0.4f, 0.35f, 1), "has a mistake: the sound keeps what it had");
      for (const GenExprDiagnostic& e : report.errors) {
        ImGui::PushID(e.line * 1000 + e.col);
        char at[48];
        std::snprintf(at, sizeof at, "line %d, col %d", e.line, e.col);
        if (textButton(at)) { w.selectFrom = offsetOf(now, e.line, e.col); w.selectTo = offsetOf(now, e.line, std::max(e.end, e.col + 1)); ImGui::SetKeyboardFocusHere(-2); }
        ImGui::SameLine(0, 10 * scale);
        ImGui::TextUnformatted(e.message.c_str());
        ImGui::PopID();
      }
    }
    for (std::size_t i = 0; i < report.params.size(); i++) {
      const GenExprKnob& k = report.params[i];
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "p%d  %s  %g to %g, starts at %g", static_cast<int>(i) + 1, k.name.c_str(), k.min, k.max, k.def);
    }

    // The sound follows once typing has paused, and only code that compiles goes in (as the web app keeps the last good program).
    if (w.changedAt != 0 && ImGui::GetTime() - w.changedAt >= kFollowSeconds) {
      w.changedAt = 0;
      if (report.ok && now != held) {
        doc.apply(std::make_unique<SetNodeData>(w.instrument, w.node, "code", Json(now), "genexpr code"), w.typing);
        w.typing = true;
        w.applied = now;
        changed = true;
      }
    }
  }
  ImGui::End();
  if (!w.open) w = GenExprWindow{};
  return changed;
}

}  // namespace wax::app
