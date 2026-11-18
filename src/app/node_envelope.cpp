#include "app/node_envelope.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/picture_slot.h"
#include "app/theme.h"

namespace wax::app {
namespace {

struct EnvelopeSet { const char *attack, *decay, *sustain, *release; };
struct NodeEnvelopes { const char* type; std::vector<EnvelopeSet> sets; };

#include "app/node_adsr_table.inc"

const NodeEnvelopes* find(const std::string& type) {
  for (const NodeEnvelopes& e : kNodeEnvelopes) if (type == e.type) return &e;
  return nullptr;
}

// The colours of the overlaid envelopes (hue, saturation, lightness as the web app has them).
ImU32 groupColour(std::size_t index, int alpha) {
  static const float hsl[4][3] = {{35, 0.85f, 0.52f}, {200, 0.70f, 0.50f}, {145, 0.55f, 0.42f}, {300, 0.50f, 0.55f}};
  const float* c = hsl[index % 4];
  const float h = c[0] / 60.0f, k = (1 - std::fabs(2 * c[2] - 1)) * c[1], x = k * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = c[2] - k / 2;
  float r = 0, g = 0, b = 0;
  if (h < 1) { r = k; g = x; } else if (h < 2) { r = x; g = k; } else if (h < 3) { g = k; b = x; } else if (h < 4) { g = x; b = k; } else if (h < 5) { r = x; b = k; } else { r = k; b = x; }
  return IM_COL32(static_cast<int>((r + m) * 255), static_cast<int>((g + m) * 255), static_cast<int>((b + m) * 255), alpha);
}

}  // namespace

bool hasEnvelopes(const std::string& nodeType) { return find(nodeType) != nullptr; }

// Attack is a straight rise; decay and release fall exponentially, and the node's times are to fall by 60 dB, so the drawing shows the part
// that matters (down to about 1%) and a sustain stage of a quarter of the whole length.
void drawNodeEnvelopes(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale) {
  const NodeEnvelopes* known = find(node.type);
  if (!known) return;
  struct Values { double a, d, s, r; };
  const auto value = [&](const char* name) {
    for (const ParamSpec& spec : type.params) if (spec.name == name) return node.param(name, spec.def);
    return 0.0;
  };
  std::vector<Values> groups;
  for (const EnvelopeSet& s : known->sets) groups.push_back({value(s.attack), value(s.decay), value(s.sustain), value(s.release)});
  double widest = 0;
  for (const Values& g : groups) widest = std::max(widest, g.a + g.d * 0.67 + g.r * 0.67);
  if (widest <= 0) return;
  const double total = widest * 1.33, hold = total * 0.25;

  const float w = kEnvelopeSize.w * scale, h = kEnvelopeSize.h * scale;
  const PictureSlot& slot = pictureSlot();
  const float x0 = slot.active ? slot.min.x : std::min(std::max(areaMin.x + 4 * scale, (tileMin.x + tileMax.x) / 2 - w / 2), std::max(areaMin.x + 4 * scale, areaMax.x - w - 4 * scale));
  const float below = tileMax.y + 16 * scale;
  const float y0 = slot.active ? slot.min.y : below + h <= areaMax.y - 4 * scale ? below : std::max(areaMin.y + 4 * scale, tileMin.y - h - 22 * scale);
  const float px = x0 + 7 * scale, pw = w - 14 * scale, py = y0 + 17 * scale, ph = 50 * scale;

  // no box: four corner ticks close around the drawing, so whatever lies under it stays visible
  const ImU32 tick = IM_COL32(128, 128, 128, 178);
  const float L = 8 * scale, l = x0 + 0.5f, r = x0 + w - 0.5f, tp = y0 + 0.5f, b = y0 + h - 0.5f;
  const ImVec2 corners[4][3] = {{{l, tp + L}, {l, tp}, {l + L, tp}}, {{r - L, tp}, {r, tp}, {r, tp + L}}, {{l, b - L}, {l, b}, {l + L, b}}, {{r - L, b}, {r, b}, {r, b - L}}};
  for (const auto& c : corners) dl->AddPolyline(c, 3, tick, 0, 1.0f);
  const float size = 11 * scale;
  dl->AddText(font, size, ImVec2(x0 + 8 * scale, y0 + 4 * scale), kText, (node.id + " envelope").c_str());
  dl->AddRectFilled(ImVec2(px, py + ph), ImVec2(px + pw, py + ph + 1), textAlpha(64));
  dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 1), textAlpha(64));

  const auto xt = [&](double ms) { return px + static_cast<float>(ms / (total + hold)) * pw; };
  const auto yv = [&](double v) { return py + ph - static_cast<float>(v) * ph; };
  for (std::size_t gi = 0; gi < groups.size(); gi++) {
    const Values& g = groups[gi];
    const double aEnd = g.a, dEnd = g.a + g.d * 0.67, sEnd = dEnd + hold, rEnd = sEnd + g.r * 0.67;
    std::vector<ImVec2> line = {ImVec2(xt(0), yv(0)), ImVec2(xt(aEnd), yv(1))};
    for (int k = 1; k <= 24; k++) { const double t = g.d * 0.67 * k / 24; line.push_back(ImVec2(xt(aEnd + t), yv(g.s + (1 - g.s) * std::pow(0.001, g.d > 0 ? t / g.d : 1)))); }
    line.push_back(ImVec2(xt(sEnd), yv(g.s)));
    for (int k = 1; k <= 24; k++) { const double t = g.r * 0.67 * k / 24; line.push_back(ImVec2(xt(sEnd + t), yv(g.s * std::pow(0.001, g.r > 0 ? t / g.r : 1)))); }
    std::vector<ImVec2> area = line;   // a faint fill under the curve
    area.push_back(ImVec2(xt(rEnd), yv(0)));
    dl->AddConcavePolyFilled(area.data(), static_cast<int>(area.size()), groupColour(gi, 36));
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), groupColour(gi, 255), 0, (groups.size() > 1 ? 1.75f : 2.5f) * std::max(1.0f, scale));
    if (gi == 0) for (const double e : {aEnd, dEnd, sEnd}) dl->AddLine(ImVec2(xt(e), py), ImVec2(xt(e), py + ph), textAlpha(76), 1.0f);   // the stage boundaries
  }
  const Values& g = groups.front();
  const auto ms = [](double v) { char t[24]; std::snprintf(t, sizeof t, v >= 100 ? "%.0f" : "%.1f", v); return std::string(t); };
  char text[96];
  std::snprintf(text, sizeof text, "A %s  D %s  S %.2f  R %s", ms(g.a).c_str(), ms(g.d).c_str(), g.s, ms(g.r).c_str());
  dl->AddText(font, size, ImVec2(px, py + ph + 6 * scale), kText, text);
}

}  // namespace wax::app
