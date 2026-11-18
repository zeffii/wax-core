#include "app/dx_window.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "app/theme.h"
#include "app/widgets.h"
#include "nodes/dx7core.h"

namespace wax::app {

namespace {

ImVec4 colour(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

std::vector<int> bankOf(const GraphNode& node) {
  std::vector<int> bank;
  if (node.data.is_object() && node.data.contains("bank") && node.data["bank"].is_array()) {
    for (const auto& b : node.data["bank"]) bank.push_back(b.is_number() ? static_cast<int>(b.get<double>()) : 0);
  }
  if (bank.size() < static_cast<std::size_t>(dx7::kVoices) * 128) bank = dx7::defaultBank();
  return bank;
}

std::string noteName(int n) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int note = n + 9;   // breakpoint 0 is A-1
  return std::string(names[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 1);
}

// Where each operator sits in the picture: carriers on the bottom row, every modulator one row above its highest receiver.
struct Placed { float x, y; };
std::array<Placed, 6> layoutAlgorithm(const dx7::Algorithm& alg, int& rows) {
  int depth[6] = {-1, -1, -1, -1, -1, -1};
  for (int c : alg.carriers) depth[c] = 0;
  for (int pass = 0; pass < 6; pass++) {
    for (const auto& m : alg.mods) if (depth[m[0]] >= 0) depth[m[1]] = std::max(depth[m[1]], depth[m[0]] + 1);
  }
  rows = 1;
  for (int d : depth) rows = std::max(rows, d + 1);
  std::array<Placed, 6> out{};
  int count[8] = {0}, seen[8] = {0};
  for (int d : depth) if (d >= 0) count[d]++;
  for (int i = 5; i >= 0; i--) {
    const int d = std::max(0, depth[i]);
    const float x = (static_cast<float>(seen[d]++) + 0.5f) / static_cast<float>(std::max(1, count[d]));
    out[static_cast<std::size_t>(i)] = {x, rows > 1 ? 1.0f - static_cast<float>(d) / static_cast<float>(rows - 1) : 0.5f};
  }
  return out;
}

// One algorithm as thin lines and numbered circles in the box (min, max). Carriers have a short line down.
void drawAlgorithm(ImDrawList* dl, int algorithmIndex, ImVec2 min, ImVec2 max, float textScale, int lit, ImU32 line) {
  const dx7::Algorithm& alg = dx7::algorithms()[static_cast<std::size_t>(algorithmIndex)];
  int rows = 1;
  const auto at = layoutAlgorithm(alg, rows);
  const float r = (textScale < 0.8f ? 3.0f : 8.0f) * ImGui::GetStyle().FontScaleDpi;
  const float w = max.x - min.x - 4 * r, h = max.y - min.y - 4 * r;
  auto pos = [&](int i) { return ImVec2(min.x + 2 * r + at[static_cast<std::size_t>(i)].x * w, min.y + 2 * r + at[static_cast<std::size_t>(i)].y * h); };
  for (const auto& m : alg.mods) dl->AddLine(pos(m[1]), pos(m[0]), line, 1.0f);
  for (int c : alg.carriers) dl->AddLine(ImVec2(pos(c).x, pos(c).y + r), ImVec2(pos(c).x, max.y), line, 1.0f);
  // feedback: a small loop on the sender
  const ImVec2 f = pos(alg.fbFrom);
  dl->AddCircle(ImVec2(f.x + r * 1.5f, f.y - r * 0.9f), r * 0.7f, kWarm, 12, 1.0f);
  for (int i = 0; i < 6; i++) {
    const bool on = i == lit;
    dl->AddCircleFilled(pos(i), r, on ? kAccent : kField, 20);
    dl->AddCircle(pos(i), r, on ? kAccent : line, 20, 1.0f);
    if (textScale < 0.8f) continue;   // thumbnails: circles only
    const std::string label = std::to_string(i + 1);
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(pos(i).x - size.x / 2, pos(i).y - size.y / 2), on ? kHiFg : kDimText, label.c_str());
  }
}

// The four-stage envelope of rates and levels as a line you can pull: the level of each point is its height, its rate sets how far
// along it sits (fast rate = short step). `rate`/`level` are 4 values 0..99. True when one moved.
bool envelopeGraph(const char* id, int rate[4], int level[4], int& held, ImVec2 size, bool pitch) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
  ImGui::InvisibleButton(id, size);
  const bool active = ImGui::IsItemActive();
  const ImGuiIO& io = ImGui::GetIO();
  float stepW[4];
  float total = 0;
  for (int i = 0; i < 4; i++) { stepW[i] = 0.25f + (99 - rate[i]) / 99.0f; total += stepW[i]; }
  const float gap = 0.4f;   // the sustain gap before the release
  const float span = total + gap;
  auto yOf = [&](int v) { return max.y - 4 - (size.y - 8) * static_cast<float>(v) / 99.0f; };
  auto levelAt = [&](int i) { return level[i]; };
  ImVec2 pts[6];
  float x = 0;
  pts[0] = ImVec2(min.x, pitch ? yOf(level[3]) : yOf(0));
  ImVec2 p[4];
  for (int i = 0; i < 4; i++) {
    x += stepW[i] + (i == 3 ? gap : 0);
    p[i] = ImVec2(min.x + size.x * x / span, yOf(levelAt(i)));
  }
  dl->AddLine(ImVec2(min.x, max.y), max, textAlpha(30));
  dl->AddLine(pts[0], p[0], kAccent, 1.5f);
  for (int i = 1; i < 4; i++) dl->AddLine(p[i - 1], p[i], kAccent, 1.5f);
  bool changed = false;
  if (ImGui::IsItemClicked()) {
    float best = 14;
    held = -1;
    for (int i = 0; i < 4; i++) {
      const float d = std::hypot(io.MousePos.x - p[i].x, io.MousePos.y - p[i].y);
      if (d < best) { best = d; held = i; }
    }
  }
  if (!active) held = -1;
  if (held >= 0) {
    const float v = std::clamp(1.0f - (io.MousePos.y - min.y - 4) / (size.y - 8), 0.0f, 1.0f);
    const int nl = static_cast<int>(std::lround(v * 99));
    if (nl != level[held]) { level[held] = nl; changed = true; }
    // dragging sideways changes the rate of the step that arrives here
    const float prevX = held == 0 ? min.x : p[held - 1].x;
    const float stepPx = std::max(2.0f, io.MousePos.x - prevX);
    const float unit = size.x / span;
    const float newW = std::clamp(stepPx / unit - (held == 3 ? gap : 0), 0.25f, 1.25f);
    const int nr = std::clamp(static_cast<int>(std::lround(99 - (newW - 0.25f) * 99)), 0, 99);
    if (nr != rate[held]) { rate[held] = nr; changed = true; }
  }
  for (int i = 0; i < 4; i++) {
    dl->AddCircleFilled(p[i], held == i ? 5.0f : 3.5f, held == i ? kWarm : kAccent);
  }
  return changed;
}

}  // namespace

void showDx(DxWindow& window, const std::string& instrument, const std::string& node) {
  if (window.instrument != instrument || window.node != node) window.op = 0;
  window.instrument = instrument;
  window.node = node;
  window.open = true;
}

namespace {

// What the sections of the window share: the voice being edited (changed in place), whether a slider is being dragged, and the scale.
struct Edit {
  DxWindow& window;
  Document& doc;
  dx7::Voice& voice;
  float scale;
  bool active = false;     // a slider or an envelope point is being dragged
  bool changed = false;    // a parameter outside the bank changed

  // an integer slider on the voice; `width` in web pixels
  void slide(const char* id, const char* label, int& value, int lo, int hi, float width = 200) {
    double v = value;
    SliderSpec spec; spec.min = lo; spec.max = hi; spec.def = lo; spec.isInt = true; spec.width = width; spec.labelWidth = 90;
    const SliderResult r = slider(id, label, v, spec);
    value = static_cast<int>(std::lround(v));
    active = active || r.active;
  }
  void next() const { ImGui::SameLine(0, 14 * scale); }
};

// voice number, feedback, algorithm and transpose in a row
void voiceRow(Edit& e, int current) {
  double voiceNo = current;
  SliderSpec spec; spec.min = 0; spec.max = dx7::kVoices - 1; spec.def = 0; spec.isInt = true; spec.width = 200; spec.labelWidth = 90;
  if (slider("dx_voice", "voice", voiceNo, spec).changed) {
    e.doc.apply(std::make_unique<SetParam>(e.window.instrument, e.window.node, "voice", voiceNo), e.window.dragging);
    e.changed = true;
  }
  e.next(); e.slide("dx_fb", "feedback", e.voice.fb, 0, 7);
  e.next(); e.slide("dx_alg", "algorithm", e.voice.alg, 0, 31);
  e.next(); e.slide("dx_trnsp", "transpose", e.voice.trnsp, 0, 48);
}

// the six operators as numbers, flat: click mutes, right-click solos; and a click on one in the picture picks it to edit
void muteSoloRow(Edit& e, int mute, int solo) {
  ImGui::TextUnformatted("operators"); ImGui::SameLine(0, 14 * e.scale);
  for (int i = 0; i < 6; i++) {
    const bool muted = (mute >> i) & 1, soloed = (solo >> i) & 1;
    ImGui::PushStyleColor(ImGuiCol_Text, soloed ? colour(kWarm) : muted ? colour(kDimText) : colour(kAccent));
    ImGui::Text("%d", i + 1);
    ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { e.doc.apply(std::make_unique<SetParam>(e.window.instrument, e.window.node, "mute", static_cast<double>(mute ^ (1 << i)))); e.changed = true; }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { e.doc.apply(std::make_unique<SetParam>(e.window.instrument, e.window.node, "solo", static_cast<double>(solo ^ (1 << i)))); e.changed = true; }
    ImGui::SameLine(0, 14 * e.scale);
  }
  ImGui::NewLine();
}

// the big picture of the algorithm, the picked operator lit, with the 32 thumbnails (current one lit) beside it
void algorithmSection(Edit& e) {
  const float scale = e.scale, pw = 150 * scale, ph = 108 * scale;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  drawAlgorithm(dl, e.voice.alg, origin, ImVec2(origin.x + pw, origin.y + ph), scale * 0.8f, e.window.op, kSoft);
  ImGui::InvisibleButton("dx_picture", ImVec2(pw, ph));
  if (ImGui::IsItemClicked()) {
    int rows = 1;
    const auto at = layoutAlgorithm(dx7::algorithms()[static_cast<std::size_t>(e.voice.alg)], rows);
    const float r = 6.5f * scale, w = pw - 4 * r, h = ph - 4 * r;
    float best = 16 * scale;
    for (int i = 0; i < 6; i++) {
      const float x = origin.x + 2 * r + at[static_cast<std::size_t>(i)].x * w, y = origin.y + 2 * r + at[static_cast<std::size_t>(i)].y * h;
      const float d = std::hypot(ImGui::GetIO().MousePos.x - x, ImGui::GetIO().MousePos.y - y);
      if (d < best) { best = d; e.window.op = i; }
    }
  }
  ImGui::SameLine(0, 12 * scale);
  ImGui::BeginGroup();
  const float tw = 30 * scale, th = 24 * scale, gap = 3 * scale;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  for (int a = 0; a < 32; a++) {
    const ImVec2 min(top.x + static_cast<float>(a % 8) * (tw + gap), top.y + static_cast<float>(a / 8) * (th + gap));
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(a);
    if (ImGui::InvisibleButton("alg", ImVec2(tw, th))) e.voice.alg = a;
    ImGui::PopID();
    drawAlgorithm(dl, a, min, ImVec2(min.x + tw, min.y + th), scale * 0.33f, -1, a == e.voice.alg ? kAccent : kDimText);
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + 4 * (th + gap)));
  ImGui::Dummy(ImVec2(1, 1));
  ImGui::EndGroup();
}

// the picked operator: its pitch and level settings and its level scaling, two sliders to a row
void operatorSection(Edit& e) {
  dx7::Op& op = e.voice.ops[e.window.op];
  ImGui::TextUnformatted("operator"); ImGui::SameLine(0, 14 * e.scale);
  for (int i = 0; i < 6; i++) {
    if (textButton(std::to_string(i + 1).c_str(), e.window.op == i)) e.window.op = i;
    ImGui::SameLine(0, 14 * e.scale);
  }
  ImGui::NewLine();
  // two sliders to a row; the curve sliders carry the name of the curve in their label
  const char* curves[] = {"-lin", "-exp", "+exp", "+lin"};
  const std::string leftCurve = std::string("left ") + curves[std::clamp(op.lc, 0, 3)], rightCurve = std::string("right ") + curves[std::clamp(op.rc, 0, 3)], breakPoint = "break " + noteName(op.bp);
  e.slide("dx_mode", "fixed freq", op.mode, 0, 1);   e.next(); e.slide("dx_fc", "coarse", op.fc, 0, 31);
  e.slide("dx_ff", "fine", op.ff, 0, 99);            e.next(); e.slide("dx_det", "detune", op.det, 0, 14);
  e.slide("dx_ol", "output level", op.ol, 0, 99);    e.next(); e.slide("dx_kvs", "velocity sens", op.kvs, 0, 7);
  e.slide("dx_rs", "rate scaling", op.rs, 0, 7);     e.next(); e.slide("dx_ams", "amp mod sens", op.ams, 0, 3);
  e.slide("dx_bp", breakPoint.c_str(), op.bp, 0, 99); e.next(); e.slide("dx_ld", "left depth", op.ld, 0, 99);
  e.slide("dx_lc", leftCurve.c_str(), op.lc, 0, 3);  e.next(); e.slide("dx_rd", "right depth", op.rd, 0, 99);
  e.slide("dx_rc", rightCurve.c_str(), op.rc, 0, 3);
}

// the picked operator's envelope, drawn and dragged
void operatorEnvelope(Edit& e, float width) {
  dx7::Op& op = e.voice.ops[e.window.op];
  int held = e.window.dragPoint;
  if (envelopeGraph("dx_env", op.r, op.l, held, ImVec2(width, 84 * e.scale), false)) e.active = true;
  e.window.dragPoint = held;
  if (held >= 0) e.active = true;
}

// the voice's LFO and its pitch envelope
void lfoSection(Edit& e) {
  ImGui::TextUnformatted("lfo");
  e.slide("dx_lfw", "wave", e.voice.lfw, 0, 5);           e.next(); e.slide("dx_lfs", "speed", e.voice.lfs, 0, 99);
  e.slide("dx_lfd", "delay", e.voice.lfd, 0, 99);         e.next(); e.slide("dx_lpmd", "pitch depth", e.voice.lpmd, 0, 99);
  e.slide("dx_pms", "pitch sens", e.voice.pms, 0, 7);     e.next(); e.slide("dx_lamd", "amp depth", e.voice.lamd, 0, 99);
  e.slide("dx_lks", "key sync", e.voice.lks, 0, 1);       e.next(); e.slide("dx_oks", "osc key sync", e.voice.oks, 0, 1);
}

// the pitch envelope (50 = no change)
void pitchEnvelope(Edit& e, float width) {
  ImGui::TextUnformatted("pitch envelope (50 = no change)");
  int held = e.window.dragPitchPoint;
  if (envelopeGraph("dx_penv", e.voice.pr, e.voice.pl, held, ImVec2(width, 64 * e.scale), true)) e.active = true;
  e.window.dragPitchPoint = held;
  if (held >= 0) e.active = true;
}

}  // namespace

bool drawDxWindow(DxWindow& window, Document& doc, const Engine& engine) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || node->type != "dvii") { window.open = false; return false; }

  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(900 * scale, 410 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(120 * scale, 40 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "DX loader  " + window.instrument + "." + window.node + "###dx";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open)) {
    const std::string address = window.instrument + "." + window.node + ".";
    auto paramNow = [&](const char* name, double def) {
      return engine.registry().slotOf(address + name) >= 0 ? engine.registry().get(address + name) : node->param(name, def);
    };
    const int current = std::clamp(static_cast<int>(std::lround(paramNow("voice", 0))), 0, dx7::kVoices - 1);
    std::vector<int> bank = bankOf(*node);
    dx7::Voice voice = dx7::unpackVoice(bank, static_cast<std::size_t>(current) * 128);
    const dx7::Voice before = voice;
    Edit e{window, doc, voice, scale};

    // everything on one screen: the voice and its algorithm on the left with the operator's envelope under it,
    // the operator's settings and the LFO on the right
    voiceRow(e, current);
    muteSoloRow(e, static_cast<int>(std::lround(paramNow("mute", 0))), static_cast<int>(std::lround(paramNow("solo", 0))));
    const float leftWidth = 400 * scale;
    ImGui::BeginGroup();
    algorithmSection(e);
    ImGui::TextUnformatted("operator envelope");
    operatorEnvelope(e, leftWidth);
    pitchEnvelope(e, leftWidth);
    ImGui::EndGroup();
    ImGui::SameLine(0, 24 * scale);
    ImGui::BeginGroup();
    operatorSection(e);
    ImGui::Dummy(ImVec2(0, 4 * scale));
    lfoSection(e);
    ImGui::EndGroup();
    changed = e.changed;
    bool active = e.active;

    // the edited voice goes back into the bank
    const bool voiceChanged = std::memcmp(&before, &voice, sizeof(dx7::Voice)) != 0;
    if (voiceChanged) {
      dx7::packVoice(voice, bank, static_cast<std::size_t>(current) * 128);
      doc.apply(std::make_unique<SetBank>(window.instrument, window.node, bank, "voice"), window.dragging);
      changed = true;
    }
    window.dragging = active;
  }
  ImGui::End();
  return changed;
}

}  // namespace wax::app
