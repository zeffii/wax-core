#include "app/cardinal_picture.h"

#include <algorithm>
#include <cmath>

#include "app/map_handle.h"
#include "app/offline_render.h"
#include "app/picture_canvas.h"
#include "app/theme.h"
#include "nodes/cardinal_view.h"

namespace wax::app {
namespace {

constexpr float kW = kCardinalWidth, kPL = 34, kPR = 8, kPlotW = kW - kPL - kPR;
constexpr float kOscH = 92, kFilterH = 104, kEnvH = 92, kLfoH = 70;
constexpr float kOscY = 0, kFilterY = kOscY + kOscH, kEnvY = kFilterY + kFilterH, kLfoY = kEnvY + kEnvH;
constexpr double kDbTop = 18, kDbBottom = -48, kSampleRate = 44100;

// the node's settings by name
struct Values {
  const GraphNode& node;
  const NodeType& type;
  double operator()(const char* name) const {
    for (const ParamSpec& spec : type.params) if (spec.name == name) return node.param(name, spec.def);
    return 0;
  }
};

std::string hzText(double v) {
  char text[16];
  if (v >= 1000) std::snprintf(text, sizeof text, v >= 10000 ? "%.0fk" : "%.1fk", v / 1000);
  else std::snprintf(text, sizeof text, "%d", static_cast<int>(std::lround(v)));
  return text;
}

// A few cycles of the oscillator section alone: filter open, nothing moving anything, unison off. `only` 1 or 2 mutes the other oscillator.
std::vector<float> oscPreview(const Values& v, int only) {
  std::vector<float> values;
  for (const ParamSpec& spec : v.type.params) values.push_back(static_cast<float>(v(spec.name.c_str())));
  const auto set = [&](const char* name, double value) { values[static_cast<std::size_t>(v.type.paramIndex(name))] = static_cast<float>(value); };
  set("ftype", 1); set("cutoff", 20000); set("res", 0); set("dist", 0); set("e2cut", 0); set("e2pitch", 0); set("e2fm", 0); set("e2pw", 0);
  set("l1amt", 0); set("l2amt", 0); set("unison", 1); set("atk", 0.1); set("sus", 1); set("vel", 0); set("level", 0.67); set("glide", 0); set("slide", 0); set("noise", 0); set("fkey", 0);
  if (only == 1) { set("mix", 0); set("ring", 0); } else if (only == 2) { set("mix", 1); set("ring", 0); }
  const double hz = 440 * std::pow(2.0, (45 - 69) / 12.0);
  return renderOffline(v.type, values, 45, 1, 3 / hz * 1000, kSampleRate, -1);
}

// the title of a panel and a faint line under it
void frame(const Canvas& c, float y, float height, const std::string& title, const std::string& right = "") {
  c.fill(0, y + height - 1, kW, 1, withAlphaF(kText, 0.1f));
  c.label(title, 6, y + 4, kText, 0.75f);
  if (!right.empty()) c.label(right, kW - kPR, y + 4, kText, 0.75f, 1);
}

void trace(const Canvas& c, const std::vector<float>& samples, float y0, float h, ImU32 colour, float width) {
  std::vector<ImVec2> points;
  const float mid = y0 + h / 2, half = h / 2 - 2;
  for (std::size_t i = 0; i < samples.size(); i++)
    points.push_back(c.at(kPL + static_cast<float>(i) / static_cast<float>(samples.size() - 1) * kPlotW, mid - std::clamp(samples[i], -1.0f, 1.0f) * half));
  c.polyline(points, colour, width);
}

void guide(const Canvas& c, float x, float y, float w) { c.fill(x, std::round(y), w, 1, withAlphaF(kText, 0.2f)); }

const ImU32 kOne = hslColour(205, 0.7f, 0.5f), kTwo = hslColour(35, 0.85f, 0.52f), kLfo1 = hslColour(145, 0.55f, 0.42f), kLfo2 = hslColour(290, 0.5f, 0.55f);

void drawOscillators(const Canvas& c, const Values& v) {
  std::string detail;
  if (v("o2sync") >= 0.5) detail += "sync";
  if (v("fm") > 0) detail += std::string(detail.empty() ? "" : " + ") + "fm";
  if (v("ring") > 0) detail += std::string(detail.empty() ? "" : " + ") + "ring";
  frame(c, kOscY, kOscH, "oscillators", std::string(cardinal::kWaves[std::clamp<long>(std::lround(v("o1wave")), 0, 4)]) + " / " + cardinal::kWaves[std::clamp<long>(std::lround(v("o2wave")), 0, 4)] + (detail.empty() ? "" : " \xc2\xb7 " + detail));
  const float y0 = kOscY + 16, h = kOscH - 22;
  guide(c, kPL, y0 + h / 2, kPlotW);
  trace(c, oscPreview(v, 1), y0, h, kOne, 1.1f);
  trace(c, oscPreview(v, 2), y0, h, kTwo, 1.1f);
  trace(c, oscPreview(v, 0), y0, h, kText, 1.8f);
}

void drawFilter(const Canvas& c, const Values& v, const GraphNode& node, ResponseHandles& handles) {
  const int type = static_cast<int>(std::clamp<long>(std::lround(v("ftype")), 0, 4));
  const double cutoff = v("cutoff"), res = v("res"), e2cut = v("e2cut");
  frame(c, kFilterY, kFilterH, "filter", std::string(cardinal::kFilters[type]) + " \xc2\xb7 " + hzText(cutoff) + " Hz \xc2\xb7 res " + std::to_string(static_cast<int>(std::lround(res * 100))) + "%");
  const float y0 = kFilterY + 16, h = kFilterH - 30;
  const double lo = std::log(20.0), hi = std::log(20000.0);
  const auto xOf = [&](double hz) { return kPL + static_cast<float>((std::log(hz) - lo) / (hi - lo)) * kPlotW; };
  const auto yOf = [&](double db) { return y0 + static_cast<float>(1 - (std::clamp(db, kDbBottom, kDbTop) - kDbBottom) / (kDbTop - kDbBottom)) * h; };
  for (const double hz : {100.0, 1000.0, 10000.0}) { c.fill(std::round(xOf(hz)), y0, 1, h, withAlphaF(kText, 0.5f)); c.label(hzText(hz), xOf(hz) + 2, y0 + h - 9, kText, 0.5f); }
  guide(c, kPL, yOf(0), kPlotW);
  const auto swept = [&](double octaves) { return xOf(std::clamp(cutoff * std::pow(2.0, octaves), 20.0, 20000.0)); };
  if (e2cut != 0) {   // the stretch the mod envelope moves the cutoff through
    const float a = swept(std::min(0.0, e2cut)), b = swept(std::max(0.0, e2cut));
    c.fill(a, y0, std::max(1.0f, b - a), h, withAlphaF(kOne, 0.13f));
  }
  std::vector<ImVec2> curve;
  for (int px = 0; px <= static_cast<int>(kPlotW); px++) {
    const double hz = std::exp(lo + px / static_cast<double>(kPlotW) * (hi - lo)), db = 20 * std::log10(std::max(1e-5, cardinal::filterResponse(type, cutoff, res, hz)));
    curve.push_back(c.at(kPL + static_cast<float>(px), yOf(db)));
  }
  c.polyline(curve, kText, 1.8f);
  c.dashed(std::round(xOf(cutoff)) + 0.5f, y0, std::round(xOf(cutoff)) + 0.5f, y0 + h, kOne, 1, 2, 3);
  // the cutoff: a dot on the curve, dragged along the axis; the sweep's far edge: dragged for how far the mod envelope takes the cutoff
  c.dot(xOf(cutoff), yOf(20 * std::log10(std::max(1e-5, cardinal::filterResponse(type, cutoff, res, cutoff)))), 3.5f, kOne);
  addMapHandle(handles, node, c.at(xOf(cutoff), yOf(20 * std::log10(std::max(1e-5, cardinal::filterResponse(type, cutoff, res, cutoff))))), c.at(kPL, 0), ImVec2(kPlotW * c.scale, 1), axis("cutoff", 20, 20000, 'g', 1));
  const float edge = swept(e2cut >= 0 ? std::max(0.0, e2cut) : std::min(0.0, e2cut));
  c.dot(edge, y0 + h - 5, 3, withAlphaF(kOne, 0.9f));
  addHandle(handles, node, "@card.e2", c.at(edge, y0 + h - 5), c.at(kPL, 0), ImVec2(kPlotW * c.scale, 1));
}

void drawEnvelopes(const Canvas& c, const Values& v) {
  const double longest = std::max(v("atk") + v("dec"), v("matk") + v("mdec"));
  const bool sustains = v("sus") > 0.02 || v("msus") > 0.02;
  const int total = static_cast<int>(std::lround(std::min(4000.0, std::max(100.0, longest * 1.2 + (sustains ? 150 : 0) + std::max(v("rel"), v("mrel")) * 0.7))));
  const int hold = sustains ? static_cast<int>(std::lround(std::min(total * 0.55, longest * 1.1 + 60))) : 100000;
  frame(c, kEnvY, kEnvH, "envelopes", std::to_string(total) + " ms");
  float lx = 82;
  for (const auto& [name, colour] : {std::pair<const char*, ImU32>{"amp", kTwo}, {"mod", kOne}}) {
    c.fill(lx, kEnvY + 5, 8, 3, colour);
    c.label(name, lx + 11, kEnvY + 2, kText, 0.75f);
    lx += 40;
  }
  const float y0 = kEnvY + 18, h = kEnvH - 28;
  guide(c, kPL, y0 + h, kPlotW);
  guide(c, kPL, y0, kPlotW);
  const std::vector<float> mod = adsrCurve(v("matk"), v("mdec"), v("msus"), v("mrel"), hold, total), amp = adsrCurve(v("atk"), v("dec"), v("sus"), v("rel"), hold, total);
  for (const auto& [curve, colour, width] : {std::tuple<const std::vector<float>*, ImU32, float>{&mod, kOne, 1.5f}, {&amp, kTwo, 2.0f}}) {
    std::vector<ImVec2> points;
    for (std::size_t i = 0; i < curve->size(); i++) points.push_back(c.at(kPL + static_cast<float>(i) / static_cast<float>(curve->size() - 1) * kPlotW, y0 + h - (*curve)[i] * h));
    c.polyline(points, colour, width);
  }
  if (hold < total) c.fill(std::round(kPL + static_cast<float>(hold) / static_cast<float>(total) * kPlotW), y0, 1, h, withAlphaF(kText, 0.5f));   // where the key comes up
}

void drawLfos(const Canvas& c, const Values& v, const GraphNode& node, ResponseHandles& handles) {
  frame(c, kLfoY, kLfoH, "lfos");
  const float half = (kPlotW - 10) / 2, y0 = kLfoY + 16, h = kLfoH - 22;
  int index = 0;
  for (const char* key : {"l1", "l2"}) {
    const std::string k = key;
    const ImU32 colour = index == 0 ? kLfo1 : kLfo2;
    const float x0 = kPL + (index == 0 ? 0 : half + 10);
    const int wave = static_cast<int>(std::clamp<long>(std::lround(v((k + "wave").c_str())), 0, 4));
    const double amount = v((k + "amt").c_str()), rate = v((k + "rate").c_str());
    char text[64];
    std::snprintf(text, sizeof text, rate < 10 ? "%s %.1f Hz > %s" : "%s %.0f Hz > %s", cardinal::kLfoWaves[wave], rate, cardinal::kLfoDests[std::clamp<long>(std::lround(v((k + "dest").c_str())), 0, 5)]);
    c.label(text, x0, kLfoY + 4, colour, 0.9f);
    guide(c, x0, y0 + h / 2, half);
    static const double held[4] = {0.4, -0.7, 0.9, -0.2};   // a few made-up steps for the random wave
    std::vector<ImVec2> points;
    const float swing = static_cast<float>(h / 2 - 2) * static_cast<float>(std::max(0.12, amount));
    for (int i = 0; i <= static_cast<int>(half); i++) {
      const double p = std::fmod(i / static_cast<double>(half), 1.0);
      points.push_back(c.at(x0 + static_cast<float>(i), y0 + h / 2 - static_cast<float>(cardinal::lfoShape(wave, p, held[std::min(3, static_cast<int>(i / half * 4))])) * swing));
    }
    c.polyline(points, withAlphaF(colour, amount > 0 ? 1.0f : 0.35f), 1.6f);
    // the wave's height is its amount: a dot at the top of the first quarter, dragged up and down
    const float dotX = x0 + half * 0.25f, dotY = y0 + h / 2 - swing;
    c.dot(dotX, dotY, 3, colour);
    addMapHandle(handles, node, c.at(dotX, dotY), ImVec2(0, c.at(0, y0 + h / 2).y), ImVec2(1, -(h / 2 - 2) * c.scale), "", axis(k + "amt", 0, 1, 'l', 0.01));
    index++;
  }
}

}  // namespace

void drawCardinal(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 origin, float scale, ResponseHandles& handles) {
  const Canvas canvas{dl, font, origin, scale};
  const Values v{node, type};
  drawOscillators(canvas, v);
  drawFilter(canvas, v, node, handles);
  drawEnvelopes(canvas, v);
  drawLfos(canvas, v, node, handles);
}

std::vector<std::pair<std::string, double>> dragCardinalHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at) {
  if (handle.param != "@card.e2") return {};
  const double fx = std::clamp(static_cast<double>((at.x - handle.gx) / handle.gw), 0.0, 1.0), hz = std::exp(std::log(20.0) + fx * (std::log(20000.0) - std::log(20.0)));
  const Values v{node, type};
  double octaves = std::log2(hz / std::max(20.0, v("cutoff")));
  for (const ParamSpec& spec : type.params) if (spec.name == "e2cut") octaves = std::clamp(octaves, spec.min, spec.max);
  return {{"e2cut", std::round(octaves * 100) / 100}};
}

}  // namespace wax::app
