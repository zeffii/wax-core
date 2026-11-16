#include "app/wavetable_window.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "app/theme.h"
#include "app/widgets.h"
#include "nodes/wave_table_data.h"
#include "project/sample_edit.h"

namespace wax::app {
namespace {

constexpr int kFrame = kTableFrame;

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

std::vector<float> framesOf(const GraphNode& node, const char* key, const std::vector<float>& fallback) {
  if (node.data.is_object() && node.data.contains(key) && node.data[key].is_string()) {
    std::vector<float> frames = decodeWaveFrames(node.data[key].get<std::string>());
    if (!frames.empty()) return frames;
  }
  return fallback;
}

Json metaOf(const GraphNode& node) { return node.data.is_object() && node.data.contains("meta") && node.data["meta"].is_object() ? node.data["meta"] : Json::object(); }

// A wave as a line in a box of the given size at the cursor; the box is left empty for the caller to draw more into.
struct Plot { ImVec2 min, max; };
Plot plotBox(const char* id, float w, float h) {
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(w, h));
  return {min, ImVec2(min.x + w, min.y + h)};
}

void trace(ImDrawList* dl, const float* a, int n, const Plot& box, float y0, float h, ImU32 colour, float width, int step = 1) {
  ImVec2 last{};
  for (int i = 0; i < n; i += step) {
    const ImVec2 p(box.min.x + static_cast<float>(i) / (n - 1) * (box.max.x - box.min.x), y0 + (1 - a[i]) * 0.5f * h);
    if (i) dl->AddLine(last, p, colour, width);
    last = p;
  }
}

// One undo step that sets `key` (null takes it away) and the node's note of where it came from.
void setWithMeta(Document& doc, const std::string& instrument, const std::string& node, const char* key, Json value, Json meta, const char* what) {
  auto edit = std::make_unique<SetNodeData>(instrument, node, key, std::move(value), what);
  edit->also("meta", meta.empty() ? Json(nullptr) : std::move(meta));
  doc.apply(std::move(edit));
}

const ImU32 kSideColour[2] = {IM_COL32(235, 160, 55, 255), IM_COL32(60, 160, 215, 255)};

}  // namespace

void showWavetable(WavetableWindow& window, const std::string& instrument, const std::string& node) {
  window.open = true; window.instrument = instrument; window.node = node;
}

bool drawWavetableWindow(WavetableWindow& window, Document& doc, const Engine& engine) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || node->type != "wavetable") { window.open = false; return false; }
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(760 * scale, 330 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(160 * scale, 100 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Wavetable  " + window.instrument + "." + window.node + "###wavetable";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar)) {
    const std::string address = window.instrument + "." + window.node + ".";
    const auto param = [&](const char* name, double def) { return engine.registry().slotOf(address + name) >= 0 ? engine.registry().get(address + name) : node->param(name, def); };
    const std::vector<float> table = framesOf(*node, "table", bundledWaveFrames());
    const std::vector<float> stored = framesOf(*node, "pair", defaultWavePair());
    std::vector<std::vector<float>> pair(2);
    pair[0].assign(stored.begin(), stored.begin() + std::min<std::size_t>(kFrame, stored.size()));
    pair[1].assign(stored.size() >= 2 * kFrame ? stored.begin() + kFrame : defaultWavePair().begin() + kFrame, stored.size() >= 2 * kFrame ? stored.begin() + 2 * kFrame : defaultWavePair().end());
    pair[0].resize(kFrame); pair[1].resize(kFrame);
    Json meta = metaOf(*node);
    const std::vector<std::string> ids = [&] { std::vector<std::string> out; if (doc.project().samples.is_object()) for (const auto& [key, value] : doc.project().samples.items()) out.push_back(key); return out; }();

    const float W = 190 * scale, tableH = 150 * scale, stripH = 34 * scale, winH = 64 * scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 ink = kText, faint = textAlpha(70);
    const auto samplePicker = [&](const char* id, std::string& chosen, bool allowDefault) {   // true when the pick changed
      bool picked = false;
      ImGui::SetNextItemWidth(W * 0.5f);
      const std::string shown = chosen.empty() ? "(default wave)" : chosen;
      if (ImGui::BeginCombo(id, shown.c_str())) {
        if (allowDefault && ImGui::Selectable("(default wave)", chosen.empty())) { chosen.clear(); picked = true; }
        for (const std::string& sid : ids) if (ImGui::Selectable(sid.c_str(), sid == chosen)) { chosen = sid; picked = true; }
        ImGui::EndCombo();
      }
      return picked;
    };

    // ---- the table ----
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Table (source 0)");
    if (textButton("bundled set")) {
      Json next = meta; next.erase("table");
      setWithMeta(doc, window.instrument, window.node, "table", Json(nullptr), next, "table");
      changed = true;
    }
    if (window.tableSample.empty() && !ids.empty()) window.tableSample = meta.contains("table") && meta["table"].contains("sample") ? meta["table"]["sample"].get<std::string>() : ids.front();
    samplePicker("##tsample", window.tableSample, false);
    ImGui::SameLine();
    static const int sizes[] = {0, 256, 512, 1024, 2048};
    ImGui::SetNextItemWidth(70 * scale);
    if (ImGui::BeginCombo("##tsize", window.tableSize ? std::to_string(window.tableSize).c_str() : "whole")) {
      for (const int s : sizes) if (ImGui::Selectable(s ? std::to_string(s).c_str() : "whole", s == window.tableSize)) window.tableSize = s;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (textButton("cut table") && doc.project().samples.is_object() && doc.project().samples.contains(window.tableSample)) {
      const EditableSample s = readSample(doc.project().samples[window.tableSample]);
      const std::vector<float> cut = tableFromSample(s.left, window.tableSize);
      if (!cut.empty()) {
        Json next = meta; next["table"] = {{"sample", window.tableSample}, {"size", window.tableSize}};
        setWithMeta(doc, window.instrument, window.node, "table", encodeWaveFrames(cut), next, "table");
        changed = true;
      }
    }
    {
      const Plot box = plotBox("##table", W, tableH);
      const int count = static_cast<int>(table.size()) / kFrame, rows = std::min(count, 14);
      const float lift = (tableH - 34 * scale) / std::max(1, rows - 1), amp = 24 * scale;
      for (int r = 0; r < rows; r++) {   // back to front: the first frame at the back (top)
        const int k = rows == 1 ? 0 : static_cast<int>(std::lround(static_cast<double>(r) * (count - 1) / (rows - 1)));
        trace(dl, table.data() + static_cast<std::size_t>(k) * kFrame, kFrame, box, box.min.y + r * lift - 4 * scale, amp, withAlpha(ink, 115), 1, 4);
      }
      if (param("source", 0) < 0.5) {   // the frame the position slider points at
        const double fp = std::clamp(param("position", 0) * (count - 1), 0.0, static_cast<double>(count - 1));
        const int k = std::min(count - 1, static_cast<int>(fp)), k1 = std::min(count - 1, k + 1);
        const float f = static_cast<float>(fp - k);
        std::vector<float> cur(kFrame);
        for (int i = 0; i < kFrame; i++) cur[static_cast<std::size_t>(i)] = table[static_cast<std::size_t>(k) * kFrame + i] * (1 - f) + table[static_cast<std::size_t>(k1) * kFrame + i] * f;
        trace(dl, cur.data(), kFrame, box, box.min.y + (rows == 1 ? 0 : static_cast<float>(fp / (count - 1)) * (rows - 1) * lift) - 4 * scale, amp, kSideColour[0], 2.5f * scale);
      }
    }
    {
      const int count = static_cast<int>(table.size()) / kFrame;
      const bool cutFromSample = meta.contains("table") && meta["table"].contains("sample");
      const std::string note = cutFromSample ? "table: " + meta["table"]["sample"].get<std::string>() + ", " + std::to_string(count) + (count == 1 ? " frame" : " frames") : "table: bundled set, " + std::to_string(count) + " waves";
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s", note.c_str());
    }
    ImGui::EndGroup();

    // ---- the A and B waves ----
    for (int side = 0; side < 2; side++) {
      ImGui::SameLine(0, 16 * scale);
      ImGui::BeginGroup();
      ImGui::PushID(side);
      const char* key = side ? "b" : "a";
      Json state = meta.contains("pair") && meta["pair"].contains(key) ? meta["pair"][key] : Json::object();
      std::string sampleId = state.contains("sample") && state["sample"].is_string() ? state["sample"].get<std::string>() : "";
      int start = state.contains("start") && state["start"].is_number() ? state["start"].get<int>() : 0;
      if (!sampleId.empty() && std::find(ids.begin(), ids.end(), sampleId) == ids.end()) sampleId.clear();
      ImGui::TextUnformatted(side ? "B" : "A");
      ImGui::SameLine();
      const auto cut = [&](const std::string& from, int at) {   // wave `side` becomes the window of `from` at `at`
        const EditableSample s = readSample(doc.project().samples[from]);
        at = std::max(0, std::min(at, std::max(0, s.frames() - kFrame)));
        std::vector<float> flat = pair[0];
        flat.insert(flat.end(), pair[1].begin(), pair[1].end());
        const std::vector<float> wave = waveWindow(s.left, at);
        std::copy(wave.begin(), wave.end(), flat.begin() + side * kFrame);
        Json next = meta;
        next["pair"][key] = {{"sample", from}, {"start", at}};
        setWithMeta(doc, window.instrument, window.node, "pair", encodeWaveFrames(flat), next, "wave");
        changed = true;
      };
      if (samplePicker("##sample", sampleId, true)) {
        if (sampleId.empty()) {   // back to the default wave: forget where it came from
          std::vector<float> flat = pair[0];
          flat.insert(flat.end(), pair[1].begin(), pair[1].end());
          const std::vector<float> fresh = defaultWavePair();
          std::copy(fresh.begin() + side * kFrame, fresh.begin() + (side + 1) * kFrame, flat.begin() + side * kFrame);
          Json next = meta;
          if (next.contains("pair")) next["pair"].erase(key);
          setWithMeta(doc, window.instrument, window.node, "pair", encodeWaveFrames(flat), next, "wave");
          changed = true;
        } else cut(sampleId, start);
      }
      if (!sampleId.empty()) {
        const EditableSample s = readSample(doc.project().samples[sampleId]);
        const int len = s.frames();
        int shown = start;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * scale);
        if (ImGui::InputInt("##start", &shown, 0, 0) && shown != start) cut(sampleId, shown);
        ImGui::SameLine();
        if (textButton("-16")) cut(sampleId, start - 16);
        ImGui::SameLine();
        if (textButton("+16")) cut(sampleId, start + 16);
        ImGui::SameLine();
        if (textButton("snap")) {   // the next rising zero crossing, so the wave starts at zero
          for (int i = std::max(1, start); i < len - kFrame; i++) if (s.left[static_cast<std::size_t>(i - 1)] < 0 && s.left[static_cast<std::size_t>(i)] >= 0) { cut(sampleId, i); break; }
        }
        const Plot strip = plotBox("##strip", W, stripH);   // the sample's outline; click to put the window there
        const float per = std::max(1.0f, static_cast<float>(len) / W);
        for (int x = 0; x < static_cast<int>(W); x++) {
          float lo = 1, hi = -1;
          const int from = static_cast<int>(x * per), to = std::min(len, static_cast<int>((x + 1) * per)), q = std::max(1, (to - from) / 16);
          for (int i = from; i < to; i += q) { lo = std::min(lo, s.left[static_cast<std::size_t>(i)]); hi = std::max(hi, s.left[static_cast<std::size_t>(i)]); }
          if (hi >= lo) dl->AddLine(ImVec2(strip.min.x + x + 0.5f, strip.min.y + stripH / 2 - hi * stripH / 2), ImVec2(strip.min.x + x + 0.5f, strip.min.y + stripH / 2 - lo * stripH / 2), withAlpha(ink, 80));
        }
        dl->AddRectFilled(ImVec2(strip.min.x + static_cast<float>(start) / len * W, strip.min.y), ImVec2(strip.min.x + std::max(2.0f, static_cast<float>(kFrame) / len * W) + static_cast<float>(start) / len * W, strip.max.y), withAlpha(kSideColour[side], 115));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) cut(sampleId, static_cast<int>(std::lround((ImGui::GetIO().MousePos.x - strip.min.x) / W * len - kFrame / 2.0)));
      } else {
        ImGui::Dummy(ImVec2(W, stripH));
      }
      const Plot win = plotBox("##win", W, winH);
      dl->AddLine(ImVec2(win.min.x, win.min.y + winH / 2), ImVec2(win.max.x, win.min.y + winH / 2), faint);
      trace(dl, pair[static_cast<std::size_t>(side)].data(), kFrame, win, win.min.y, winH, kSideColour[side], 2 * scale);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s", sampleId.empty() ? "a default wave; pick a sample to cut one" : ("frames " + std::to_string(start) + " to " + std::to_string(start + kFrame)).c_str());
      ImGui::PopID();
      ImGui::EndGroup();
    }

    // ---- the blend ----
    ImGui::SameLine(0, 16 * scale);
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Blend (source 1)");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "A to B by mix");
    const Plot box = plotBox("##blend", W, winH);
    const float mix = static_cast<float>(std::clamp(param("mix", 0), 0.0, 1.0));
    dl->AddLine(ImVec2(box.min.x, box.min.y + winH / 2), ImVec2(box.max.x, box.min.y + winH / 2), faint);
    trace(dl, pair[0].data(), kFrame, box, box.min.y, winH, withAlpha(kSideColour[0], 100), 1, 2);
    trace(dl, pair[1].data(), kFrame, box, box.min.y, winH, withAlpha(kSideColour[1], 100), 1, 2);
    std::vector<float> blend(kFrame);
    for (int i = 0; i < kFrame; i++) blend[static_cast<std::size_t>(i)] = pair[0][static_cast<std::size_t>(i)] * (1 - mix) + pair[1][static_cast<std::size_t>(i)] * mix;
    trace(dl, blend.data(), kFrame, box, box.min.y, winH, ink, 2 * scale);
    ImGui::EndGroup();
  }
  ImGui::End();
  return changed;
}

}  // namespace wax::app
