#include "app/node_help.h"
#include "app/native_additions.h"

#include <cstdio>
#include <vector>

#include <SDL3/SDL_misc.h>
#include "app/theme.h"
#include "app/widgets.h"
#include "core/node_registry.h"
#include "imgui.h"

namespace wax::app {
namespace {

struct HelpLink { const char* name; const char* url; const char* note; };
struct HelpRow { const char* key; const char* value; };
struct HelpSection { const char* title; std::vector<const char*> text; std::vector<HelpRow> rows; };
struct NodeHelp {
  const char* type;
  const char* summary;
  std::vector<const char*> best, limits;
  std::vector<HelpLink> links;
  std::vector<HelpSection> sections;
};
struct ParamHint { const char* type; const char* name; const char* text; };

#include "app/node_help_table.inc"

const NodeHelp* pageFor(const std::string& type) {
  for (const NodeHelp& page : kNodeHelp) if (type == page.type) return &page;
  return nullptr;
}

const char* kindName(PortKind kind) {
  switch (kind) {
    case PortKind::Control: return "control";
    case PortKind::Param: return "parameter";
    case PortKind::Stereo: return "stereo";
    default: return "audio";
  }
}

std::string portList(const std::vector<std::string>& names, const NodeType& type) {
  std::string out;
  for (const std::string& name : names) {
    const auto kind = type.kinds.find(name);
    out += (out.empty() ? "" : ", ") + name + " (" + kindName(kind == type.kinds.end() ? PortKind::Audio : kind->second) + ")";
  }
  return out.empty() ? "none" : out;
}

std::string number(double v) {
  char text[32];
  if (v == static_cast<long long>(v)) std::snprintf(text, sizeof text, "%lld", static_cast<long long>(v));
  else std::snprintf(text, sizeof text, "%.3g", v);
  return text;
}

void heading(const char* text) {
  ImGui::Spacing();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "%s", text);
}

void bullets(const std::vector<const char*>& items) {
  for (const char* item : items) {
    ImGui::Bullet();
    ImGui::TextWrapped("%s", item);
  }
}

}  // namespace

std::string paramHint(const std::string& type, const std::string& param) {
  const std::string native = nativeParamHint(type, param);   // (what only native has comes first)
  if (!native.empty()) return native;
  for (const ParamHint& hint : kParamHints) if (type == hint.type && param == hint.name) return hint.text;
  return std::string();
}

void showNodeHelp(NodeHelpWindow& window, const std::string& type) {
  window.open = true;
  window.type = type;
}

void drawNodeHelp(NodeHelpWindow& window) {
  if (!window.open) return;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(520 * scale, 560 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + 340 * scale, ImGui::GetMainViewport()->WorkPos.y + 60 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Help: " + window.type + "###nodehelp";
  if (!ImGui::Begin(title.c_str(), &window.open)) { ImGui::End(); return; }
  const NodeType* type = builtinNodes().find(window.type);
  const NodeHelp* page = pageFor(window.type);
  const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(kDimText);
  if (page) {
    ImGui::TextWrapped("%s", page->summary);
    if (!page->best.empty()) { heading("Getting the most out of it"); bullets(page->best); }
    if (!page->limits.empty()) { heading("Limits"); bullets(page->limits); }
    if (!page->links.empty()) {
      heading("Links");
      for (const HelpLink& link : page->links) {
        if (textButton(link.name)) SDL_OpenURL(link.url);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", link.url);
        if (*link.note) { ImGui::SameLine(); ImGui::TextColored(dim, "%s", link.note); }
      }
    }
    for (const HelpSection& section : page->sections) {
      heading(section.title);
      for (const char* text : section.text) ImGui::TextWrapped("%s", text);
      for (const HelpRow& row : section.rows) {
        ImGui::TextColored(dim, "%s", row.key);
        ImGui::Indent(14 * scale);
        ImGui::TextWrapped("%s", row.value);
        ImGui::Unindent(14 * scale);
      }
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextWrapped("There is no written page for this node yet. This is its list of ports and parameters.");
    ImGui::PopStyleColor();
  }
  if (type) {
    heading("Ports");
    ImGui::TextWrapped("In: %s. Out: %s.", portList(type->inputs, *type).c_str(), portList(type->outputs, *type).c_str());
    if (!type->params.empty()) {
      heading("Parameters");
      for (const ParamSpec& spec : type->params) {
        ImGui::TextUnformatted(spec.name.c_str());
        ImGui::Indent(14 * scale);
        const std::string hint = paramHint(window.type, spec.name);
        if (!hint.empty()) ImGui::TextWrapped("%s", hint.c_str());
        ImGui::TextColored(dim, "range %s to %s, default %s%s", number(spec.min).c_str(), number(spec.max).c_str(), number(spec.def).c_str(), spec.isInt ? ", whole numbers" : "");
        ImGui::Unindent(14 * scale);
      }
    }
  }
  ImGui::End();
}

}  // namespace wax::app
