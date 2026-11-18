#include "app/node_response.h"
#include "app/node_pictures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/picture_slot.h"
#include "app/theme.h"

namespace wax::app {
namespace {

constexpr double kPi = 3.14159265358979323846, kSampleRate = 48000, kFLo = 20, kFHi = 20000, kRangeDb = 18;
constexpr float kWidth = kResponseSize.w, kHeight = kResponseSize.h;

enum class Kind { LowShelf, Peak, HighShelf, Highpass, Lowpass };
struct Coef { double b0, b1, b2, a1, a2; };

// RBJ biquads, as the equaliser node has them.
Coef biquad(Kind kind, double f, double gainDb, double q) {
  const double w = 2 * kPi * std::min(std::max(f, 5.0), kSampleRate * 0.45) / kSampleRate, cw = std::cos(w), sw = std::sin(w);
  const double A = std::pow(10, gainDb / 40), alpha = sw / (2 * q);
  double b0, b1, b2, a0, a1, a2;
  if (kind == Kind::Peak) {
    b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
  } else if (kind == Kind::LowShelf || kind == Kind::HighShelf) {
    const double t = 2 * std::sqrt(A) * alpha, s = kind == Kind::LowShelf ? 1 : -1;
    b0 = A * ((A + 1) - s * (A - 1) * cw + t);
    b1 = s * 2 * A * ((A - 1) - s * (A + 1) * cw);
    b2 = A * ((A + 1) - s * (A - 1) * cw - t);
    a0 = (A + 1) + s * (A - 1) * cw + t;
    a1 = -s * 2 * ((A - 1) + s * (A + 1) * cw);
    a2 = (A + 1) + s * (A - 1) * cw - t;
  } else if (kind == Kind::Highpass) {
    b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  } else {
    b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  }
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double magnitudeDb(const Coef& c, double f) {
  const double w = 2 * kPi * f / kSampleRate, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
  const double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
  const double dr = 1 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
  return 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di) + 1e-30);
}

struct Band { Coef coef; double f; int band; bool cut, off; };

double valueOf(const GraphNode& node, const NodeType& type, const std::string& name) {
  for (const ParamSpec& spec : type.params) if (spec.name == name) return node.param(name, spec.def);
  return 0;
}

std::vector<Band> bandsOf(const GraphNode& node, const NodeType& type, const std::string& skipGain = {}) {
  static const Kind kinds[5] = {Kind::LowShelf, Kind::Peak, Kind::Peak, Kind::Peak, Kind::HighShelf};
  std::vector<Band> out;
  const double lowcut = valueOf(node, type, "lowcut"), highcut = valueOf(node, type, "highcut");
  const double root = 0.7071067811865476;
  if (lowcut > 20.5) out.push_back({biquad(Kind::Highpass, lowcut, 0, root), lowcut, 0, true, false});
  for (int k = 1; k <= 5; k++) {
    const std::string n = std::to_string(k);
    const double gain = "g" + n == skipGain ? 0 : valueOf(node, type, "g" + n), f = valueOf(node, type, "f" + n);
    out.push_back({biquad(kinds[k - 1], f, gain, valueOf(node, type, "q" + n)), f, k, false, std::fabs(gain) < 0.01});
  }
  if (highcut < 19990) out.push_back({biquad(Kind::Lowpass, highcut, 0, root), highcut, 0, true, false});
  return out;
}

double totalDb(const std::vector<Band>& bands, double f) {
  double t = 0;
  for (const Band& b : bands) if (!b.off) t += magnitudeDb(b.coef, f);
  return t;
}

ImU32 bandColour(int index) {
  static const ImU32 colours[4] = {IM_COL32(235, 140, 40, 255), IM_COL32(38, 148, 217, 255), IM_COL32(49, 163, 100, 255), IM_COL32(198, 89, 198, 255)};
  return colours[index % 4];
}

}  // namespace

bool hasResponse(const std::string& nodeType) { return nodeType == "eq"; }

void drawNodeResponse(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, ResponseHandles& handles) {
  const float w = kWidth * scale, h = kHeight * scale;
  const PictureSlot& slot = pictureSlot();
  const float x0 = slot.active ? slot.min.x : std::min(std::max(areaMin.x + 4 * scale, (tileMin.x + tileMax.x) / 2 - w / 2), std::max(areaMin.x + 4 * scale, areaMax.x - w - 4 * scale));
  const float below = tileMax.y + 16 * scale;
  const float y0 = slot.active ? slot.min.y : below + h <= areaMax.y - 4 * scale ? below : std::max(areaMin.y + 4 * scale, tileMin.y - h - 22 * scale);
  const float px = x0 + 30 * scale, pw = w - 42 * scale, py = y0 + 20 * scale, ph = h - 44 * scale;
  const auto xf = [&](double f) { return px + static_cast<float>(std::log(f / kFLo) / std::log(kFHi / kFLo)) * pw; };
  const auto yd = [&](double db) { return py + ph / 2 - static_cast<float>(std::max(-kRangeDb, std::min(kRangeDb, db)) / kRangeDb) * (ph / 2); };
  handles.node = node.id; handles.px = px; handles.py = py; handles.pw = pw; handles.ph = ph;

  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), kBg);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(128, 128, 128, 41));
  dl->AddRect(ImVec2(x0 + 0.5f, y0 + 0.5f), ImVec2(x0 + w - 0.5f, y0 + h - 0.5f), IM_COL32(128, 128, 128, 178));
  const float size = 11 * scale;
  dl->AddText(font, size, ImVec2(x0 + 8 * scale, y0 + 4 * scale), kText, (node.id + " response").c_str());
  for (const double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
    const float x = std::round(xf(f)) + 0.5f;
    dl->AddLine(ImVec2(x, py), ImVec2(x, py + ph), textAlpha(f == 100 || f == 1000 || f == 10000 ? 71 : 31));
  }
  for (const int db : {-18, -12, -6, 0, 6, 12, 18}) {
    const float y = std::round(yd(db)) + 0.5f;
    dl->AddLine(ImVec2(px, y), ImVec2(px + pw, y), textAlpha(db == 0 ? 115 : 31));
  }
  for (const int db : {-12, 0, 12}) {
    char t[8]; std::snprintf(t, sizeof t, "%s%d", db > 0 ? "+" : "", db);
    dl->AddText(font, size, ImVec2(x0 + 5 * scale, yd(db) - size / 2), textAlpha(190), t);
  }
  for (const auto& [f, name] : {std::pair<double, const char*>{100, "100"}, {1000, "1k"}, {10000, "10k"}}) dl->AddText(font, size, ImVec2(xf(f) - 6 * scale, py + ph + 4 * scale), textAlpha(190), name);

  const std::vector<Band> bands = bandsOf(node, type);
  const int steps = std::max(8, static_cast<int>(pw));
  const auto freq = [&](int i) { return kFLo * std::pow(kFHi / kFLo, static_cast<double>(i) / steps); };
  for (std::size_t bi = 0; bi < bands.size(); bi++) {   // each band's own curve, thin
    if (bands[bi].off) continue;
    std::vector<ImVec2> line;
    for (int i = 0; i <= steps; i += 2) line.push_back(ImVec2(px + static_cast<float>(i) / steps * pw, yd(magnitudeDb(bands[bi].coef, freq(i)))));
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), withAlpha(bandColour(static_cast<int>(bi)), 153), 0, 1.0f);
  }
  std::vector<ImVec2> sum;
  for (int i = 0; i <= steps; i++) sum.push_back(ImVec2(px + static_cast<float>(i) / steps * pw, yd(totalDb(bands, freq(i)))));
  std::vector<ImVec2> fill = sum;   // the combined response: filled down to the 0 dB line, then the line itself
  fill.push_back(ImVec2(px + pw, yd(0))); fill.push_back(ImVec2(px, yd(0)));
  dl->AddConcavePolyFilled(fill.data(), static_cast<int>(fill.size()), textAlpha(46));
  dl->AddPolyline(sum.data(), static_cast<int>(sum.size()), kText, 0, 2.25f * std::max(1.0f, scale));
  for (std::size_t bi = 0; bi < bands.size(); bi++) {   // a dot per band, at its frequency and its gain on the combined curve
    if (bands[bi].cut) continue;
    const ImVec2 at(xf(bands[bi].f), yd(totalDb(bands, bands[bi].f)));
    dl->AddCircleFilled(at, 4.5f * scale, bandColour(static_cast<int>(bi)));
    dl->AddText(font, size, ImVec2(at.x - 3 * scale, at.y - 17 * scale), kText, std::to_string(bands[bi].band).c_str());
    handles.list.push_back({node.id, "f" + std::to_string(bands[bi].band), bands[bi].band, at});
  }
  for (const char* name : {"lowcut", "highcut"}) {   // a small triangle at the top edge: drag it sideways to set a cut
    const double f = valueOf(node, type, name);
    const bool on = std::string(name) == "lowcut" ? f > 20.5 : f < 19990;
    const ImVec2 at(xf(f), py + 7 * scale);
    dl->AddTriangleFilled(ImVec2(at.x - 5 * scale, at.y - 5 * scale), ImVec2(at.x + 5 * scale, at.y - 5 * scale), ImVec2(at.x, at.y + 4 * scale), withAlpha(kText, on ? 230 : 90));
    handles.list.push_back({node.id, name, 0, at});
  }
  dl->AddText(font, size, ImVec2(x0 + 5 * scale, y0 + h - 18 * scale), textAlpha(190), "dB");
}

std::vector<std::pair<std::string, double>> dragResponse(const ResponseHandles& handles, const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at) {
  const auto clampTo = [&](const std::string& name, double v) {
    for (const ParamSpec& spec : type.params) if (spec.name == name) return std::min(spec.max, std::max(spec.min, v));
    return v;
  };
  if (!handle.param.empty() && handle.param[0] == '@') return dragPictureHandle(handle, node, type, at);
  const double freq = kFLo * std::pow(kFHi / kFLo, std::min(1.0, std::max(0.0, static_cast<double>((at.x - handles.px) / handles.pw))));
  std::vector<std::pair<std::string, double>> changes = {{handle.param, clampTo(handle.param, freq)}};
  if (handle.band == 0) return changes;
  // the dot sits on the combined curve, so the gain is what is left once the other bands are counted
  const std::string gain = "g" + std::to_string(handle.band);
  const double others = totalDb(bandsOf(node, type, gain), clampTo(handle.param, freq));
  const double want = (handles.py + handles.ph / 2 - at.y) / (handles.ph / 2) * kRangeDb;
  changes.push_back({gain, clampTo(gain, std::round((want - others) * 10) / 10)});
  return changes;
}

}  // namespace wax::app

namespace wax::app {

void beginHandleDrag(ResponseHandles& handles, ImVec2 mouse) { handles.grabAt = mouse; handles.fine = false; }

ImVec2 handleDragPoint(ResponseHandles& handles, ImVec2 mouse, bool shift) {
  if (!shift) { handles.fine = false; handles.grabAt = mouse; return mouse; }
  if (!handles.fine) { handles.fine = true; handles.fineMouse = mouse; handles.fineAt = handles.grabAt; }
  handles.grabAt = ImVec2(handles.fineAt.x + (mouse.x - handles.fineMouse.x) * 0.1f, handles.fineAt.y + (mouse.y - handles.fineMouse.y) * 0.1f);
  return handles.grabAt;
}

std::vector<std::pair<std::string, double>> resetHandle(const ResponseHandles& handles, const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type) {
  std::vector<std::pair<std::string, double>> defaults;
  const bool button = handle.once || handle.param == "@draw" || handle.param.rfind("@set:", 0) == 0 || handle.param.rfind("@flip:", 0) == 0 || handle.param.rfind("@nudge:", 0) == 0;
  if (button) return defaults;
  for (const auto& [name, value] : dragResponse(handles, handle, node, type, handle.pos))
    for (const ParamSpec& spec : type.params) if (spec.name == name) defaults.push_back({name, spec.def});
  return defaults;
}

}  // namespace wax::app
