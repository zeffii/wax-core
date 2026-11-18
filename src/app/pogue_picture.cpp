#include "app/pogue_picture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>

#include "app/picture_canvas.h"
#include "app/theme.h"

namespace wax::app {
namespace {

// the sizes of the parts and of what they hold, in web pixels (as pogueview.js)
constexpr float kVoicesW = 560, kPadLeft = 40, kPadRight = 14, kTop = 22, kTrace = 150, kVoicesH = kTop + kTrace + 22;
constexpr float kSmallW = 190, kSmallTop = 22, kStrip = 92, kSmallH = kSmallTop + kStrip + 22;
constexpr float kPatternW = 330, kPatternTop = 22, kPatternSteps = 22;
constexpr float kGap = 10;
constexpr int kSeconds = 6, kKept = 180;
constexpr double kRatioMax = 12;   // the top of the detector's (log) axis

const ImU32 kAmber = IM_COL32(250, 200, 70, 255), kGreen = IM_COL32(60, 205, 168, 255), kOrangeInk = IM_COL32(245, 160, 60, 255), kRed = IM_COL32(235, 90, 75, 255), kCyan = IM_COL32(60, 190, 225, 255);

ImU32 voiceColour(int voice, int count) {   // orange to cyan, so no purples
  const float hue = 28 + (count > 1 ? static_cast<float>(voice) / (count - 1) : 0.0f) * 170, h = hue / 60, k = (1 - std::fabs(2 * 0.58f - 1)) * 0.78f, x = k * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = 0.58f - k / 2;
  float r = 0, g = 0, b = 0;
  if (h < 1) { r = k; g = x; } else if (h < 2) { r = x; g = k; } else if (h < 3) { g = k; b = x; } else if (h < 4) { g = x; b = k; } else { r = x; b = k; }
  return IM_COL32(static_cast<int>((r + m) * 255), static_cast<int>((g + m) * 255), static_cast<int>((b + m) * 255), 255);
}

double ratioThreshold(double sensitivity) { return 1.2 + 8 * std::pow(1 - sensitivity, 2); }   // the same as the node's
double sensitivityOf(double ratio) { return std::clamp(1 - std::sqrt(std::max(0.0, ratio - 1.2) / 8), 0.02, 1.0); }

double valueOf(const GraphNode& node, const NodeType& type, const std::string& name) {
  for (const ParamSpec& spec : type.params) if (spec.name == name) return node.param(name, spec.def);
  return 0;
}

// the delay axis: 0 to a little over the longest the voices can reach
double axisMax(const GraphNode& node, const NodeType& type) {
  return std::max(8.0, std::ceil(std::max(valueOf(node, type, "mindelay"), valueOf(node, type, "maxdelay")) * 1.25 / 2) * 2);
}

// gain of a 2nd-order high or low pass at f (1 at the pass band), the same maths as the node's filters
double passGain(double f, double cut, double q, bool high, double sampleRate) {
  const double pi = 3.14159265358979323846;
  const double w = std::tan(pi * std::min(f, sampleRate * 0.499) / sampleRate) / std::tan(pi * std::min(cut, sampleRate * 0.45) / sampleRate), k = 1 / q;
  return (high ? w * w : 1) / std::sqrt(std::pow(1 - w * w, 2) + std::pow(k * w, 2));
}

// ---- the reports from the audio thread ----
// The node publishes: [report number, voices, level, ratio, fired, left delays (ms)..., right delays...]. A report is kept once.
struct Report { std::vector<float> left, right; float level = 0, ratio = 1; int fired = 0; };
struct History { std::deque<Report> reports; float lastNumber = -1; bool live = false; };

History& updatedHistory(const GraphNode& node, const PictureTiming& timing) {
  static std::map<std::string, History> histories;
  History& history = histories[timing.instrument + "/" + node.id];
  const std::shared_ptr<SpectrumProbe> probe = timing.probeOf ? timing.probeOf(timing.instrument, node.id) : nullptr;
  if (!probe) return history;
  probe->watch();
  std::vector<float> in, out;
  if (!probe->read(in, out) || in.size() < 5 || in[0] == history.lastNumber) return history;
  const std::size_t count = static_cast<std::size_t>(in[1]);
  if (in.size() < 5 + 2 * count) return history;
  history.lastNumber = in[0];
  Report report;
  report.level = in[2]; report.ratio = in[3]; report.fired = static_cast<int>(in[4]);
  report.left.assign(in.begin() + 5, in.begin() + 5 + static_cast<std::ptrdiff_t>(count));
  report.right.assign(in.begin() + 5 + static_cast<std::ptrdiff_t>(count), in.begin() + 5 + 2 * static_cast<std::ptrdiff_t>(count));
  history.reports.push_back(std::move(report));
  while (history.reports.size() > static_cast<std::size_t>(kKept)) history.reports.pop_front();
  history.live = true;
  return history;
}

void drawVoices(const Canvas& c, const GraphNode& node, const NodeType& type, const History& history, ResponseHandles& handles) {
  const double lo = std::min(valueOf(node, type, "mindelay"), valueOf(node, type, "maxdelay")), hi = std::max(valueOf(node, type, "mindelay"), valueOf(node, type, "maxdelay"));
  const int count = static_cast<int>(std::lround(valueOf(node, type, "voices"))), stereo = static_cast<int>(std::lround(valueOf(node, type, "stereo")));
  const double top = axisMax(node, type), onset = valueOf(node, type, "onset");
  const float pw = kVoicesW - kPadLeft - kPadRight;
  const auto dy = [&](double ms) { return kTop + kTrace - static_cast<float>(std::clamp(ms / top, 0.0, 1.0)) * kTrace; };
  const ImU32 ink = kText;
  c.label("VOICES \xc2\xb7 where each one reads the delay line (ms)", kPadLeft, 6, ink);
  const int step = top <= 16 ? 2 : top <= 30 ? 5 : 10;
  for (int ms = 0; ms <= top; ms += step) {
    c.line(kPadLeft, std::round(dy(ms)) + 0.5f, kVoicesW - kPadRight, std::round(dy(ms)) + 0.5f, withAlphaF(ink, 0.12f));
    c.label(std::to_string(ms), kPadLeft - 5, dy(ms) - 4, ink, 0.6f, 1);
  }
  c.fill(kPadLeft, dy(hi), pw, dy(lo) - dy(hi), withAlphaF(kAmber, 0.1f));
  for (const double ms : {lo, hi}) c.dashed(kPadLeft, std::round(dy(ms)) + 0.5f, kVoicesW - kPadRight, std::round(dy(ms)) + 0.5f, withAlphaF(kAmber, 0.8f), 1.2f, 4, 3);
  c.label("longest " + number(hi) + " ms", kVoicesW - kPadRight - 84, dy(hi) - 11, kAmber, 0.95f);
  c.label("shortest " + number(lo) + " ms", kVoicesW - kPadRight - 90, dy(lo) + 3, kAmber, 0.95f);
  for (int s = 0; s <= kSeconds; s++) c.label(s == kSeconds ? "now" : "-" + std::to_string(kSeconds - s) + "s", kPadLeft + static_cast<float>(s) / kSeconds * pw - (s == kSeconds ? 8 : 6), kTop + kTrace + 3, ink, 0.45f);

  const auto& reports = history.reports;
  const int stored = static_cast<int>(reports.size());
  const auto hx = [&](int k) { return kPadLeft + pw - static_cast<float>(stored - 1 - k) / kKept * pw; };
  const bool patternOn = std::lround(valueOf(node, type, "pattern")) > 0;
  const ImVec2 rectMin = c.at(kPadLeft, kTop), rectSize(pw * c.scale, kTrace * c.scale);
  if (onset > 0) {   // the fast/slow level ratio on a log scale of its own, the sensitivity line
    const auto ry = [&](double ratio) { return kTop + kTrace - static_cast<float>(std::clamp(std::log(std::max(1.0, ratio)) / std::log(kRatioMax), 0.0, 1.0)) * kTrace; };
    std::vector<ImVec2> line;
    for (int k = 0; k < stored; k++) line.push_back(c.at(hx(k), ry(reports[static_cast<std::size_t>(k)].ratio)));
    if (line.size() > 1) c.dl->AddPolyline(line.data(), static_cast<int>(line.size()), withAlphaF(kAmber, 0.45f), 0, std::max(1.0f, 1.2f * c.scale));
    const float threshold = ry(ratioThreshold(onset));
    c.dashed(kPadLeft, std::round(threshold) + 0.5f, kVoicesW - kPadRight, std::round(threshold) + 0.5f, withAlphaF(kGreen, 0.9f), 1.2f, 5, 4);
    c.label("onset " + number(onset, 2), kPadLeft + 4, threshold - 11, kGreen, 0.95f);
    addLineHandles(handles, node, "@pogonset", c, threshold, kPadLeft, kVoicesW - kPadRight, rectMin, rectSize);
  }
  for (int k = 0; k < stored; k++) {   // a line through the picture for each trigger that fired
    const int fired = reports[static_cast<std::size_t>(k)].fired;
    for (const auto& [bit, colour, show] : {std::tuple<int, ImU32, bool>{1, kGreen, onset > 0}, {2, kCyan, onset > 0}, {4, kOrangeInk, patternOn}})
      if ((fired & bit) && show) c.line(std::round(hx(k)) + 0.5f, kTop, std::round(hx(k)) + 0.5f, kTop + kTrace, withAlphaF(colour, 0.55f));
  }
  const auto trace = [&](bool right, float width, bool dashes, float alpha) {   // the right side (wide mode) first, thin and dashed, so the left lines lie over it
    for (int v = 0; v < count; v++) {
      std::vector<ImVec2> line;
      for (int k = 0; k < stored; k++) {
        const std::vector<float>& side = right ? reports[static_cast<std::size_t>(k)].right : reports[static_cast<std::size_t>(k)].left;
        if (static_cast<std::size_t>(v) < side.size()) line.push_back(c.at(hx(k), dy(side[static_cast<std::size_t>(v)])));
      }
      const ImU32 colour = withAlphaF(voiceColour(v, count), alpha);
      if (!dashes) { if (line.size() > 1) c.dl->AddPolyline(line.data(), static_cast<int>(line.size()), colour, 0, std::max(1.0f, width * c.scale)); continue; }
      for (std::size_t i = 0; i + 1 < line.size(); i += 2) c.dl->AddLine(line[i], line[i + 1], colour, std::max(1.0f, width * c.scale));   // (every other piece: dashed)
    }
  };
  if (stereo == 1) trace(true, 1, true, 0.6f);
  trace(false, 1.8f, false, 1);
  if (stored) {   // a dot on each voice's latest place
    const Report& last = reports.back();
    for (int v = 0; v < count; v++) if (static_cast<std::size_t>(v) < last.left.size()) c.dot(hx(stored - 1), dy(last.left[static_cast<std::size_t>(v)]), 3.2f, voiceColour(v, count));
  }
  if (!history.live) c.label("play something through it to watch the voices", kPadLeft + pw / 2 - 118, kTop + kTrace / 2 - 4, ink, 0.5f);
  addLineHandles(handles, node, "@pogmin", c, dy(valueOf(node, type, "mindelay")), kPadLeft, kVoicesW - kPadRight, rectMin, rectSize);
  addLineHandles(handles, node, "@pogmax", c, dy(valueOf(node, type, "maxdelay")), kPadLeft, kVoicesW - kPadRight, rectMin, rectSize);
}

// ---- the filters: log frequency 20 Hz to 20 kHz, -36 to +12 dB ----
float filterX(double f) { return 8 + static_cast<float>((std::log2(f) - std::log2(20.0)) / (std::log2(20000.0) - std::log2(20.0))) * (kSmallW - 16); }
float filterY(double db) { return kSmallTop + kStrip - static_cast<float>((std::clamp(db, -36.0, 12.0) + 36) / 48) * kStrip; }

void drawFilters(const Canvas& c, const GraphNode& node, const NodeType& type, ResponseHandles& handles) {
  const double hpCut = valueOf(node, type, "hpcut"), lpCut = valueOf(node, type, "lpcut"), hpQ = valueOf(node, type, "hpq"), lpQ = valueOf(node, type, "lpq");
  c.label("FILTERS", 8, 4, kText);
  for (const int f : {100, 1000, 10000}) {
    c.line(std::round(filterX(f)) + 0.5f, kSmallTop, std::round(filterX(f)) + 0.5f, kSmallTop + kStrip, withAlphaF(kText, 0.13f));
    c.label(f >= 1000 ? std::to_string(f / 1000) + "k" : std::to_string(f), filterX(f) - 6, kSmallTop + kStrip + 3, kText, 0.45f);
  }
  c.line(8, std::round(filterY(0)) + 0.5f, kSmallW - 8, std::round(filterY(0)) + 0.5f, withAlphaF(kText, 0.13f));
  std::vector<ImVec2> curve;
  for (int i = 0; i <= 120; i++) {
    const double f = 20 * std::pow(1000.0, i / 120.0), gain = 20 * std::log10(std::max(1e-4, passGain(f, hpCut, hpQ, true, 48000) * passGain(f, lpCut, lpQ, false, 48000)));
    curve.push_back(c.at(filterX(f), filterY(gain)));
  }
  c.dl->AddPolyline(curve.data(), static_cast<int>(curve.size()), kGreen, 0, std::max(1.0f, 1.6f * c.scale));
  const ImVec2 rectMin = c.at(8, kSmallTop), rectSize((kSmallW - 16) * c.scale, kStrip * c.scale);
  for (const auto& [cut, name, side] : {std::tuple<double, const char*, int>{hpCut, "hp", 1}, {lpCut, "lp", 0}}) {
    c.dashed(std::round(filterX(cut)) + 0.5f, kSmallTop, std::round(filterX(cut)) + 0.5f, kSmallTop + kStrip, withAlphaF(kAmber, 0.8f), 1, 2, 3);
    c.dot(filterX(cut), filterY(-3), 3, withAlphaF(kAmber, 0.8f));
    const int where = side == 0 && filterX(cut) + 62 > kSmallW ? 1 : side;   // keep the label inside the picture
    c.label(std::string(name) + " " + hertz(cut), filterX(cut) + (where == 1 ? -4 : 4), kSmallTop + 2, kAmber, 1, where);
    addHandle(handles, node, std::string("@pogf") + name, c.at(filterX(cut), filterY(-3)), rectMin, rectSize);
    for (float y = kSmallTop + 6; y < kSmallTop + kStrip; y += 14) addHandle(handles, node, std::string("@pogf") + name, c.at(filterX(cut), y), rectMin, rectSize);
  }
}

// ---- saturation, then bit reduction: what comes out for what goes in ----
void drawShaper(const Canvas& c, const GraphNode& node, const NodeType& type) {
  const double saturation = valueOf(node, type, "saturation"), degrade = valueOf(node, type, "degrade"), levels = degrade > 0 ? std::pow(2, 16 - 14 * degrade - 1) : 0, drive = 1 + 9 * saturation;
  const float x0 = 8, w = kSmallW - 16;
  c.label("SATURATE + BITS" + (degrade > 0 ? " \xc2\xb7 " + number(16 - 14 * degrade) + " bits" : std::string()), 8, 4, kText);
  c.rect(x0 + 0.5f, kSmallTop + 0.5f, w, kStrip, withAlphaF(kText, 0.25f));
  c.line(x0, kSmallTop + kStrip / 2 + 0.5f, x0 + w, kSmallTop + kStrip / 2 + 0.5f, withAlphaF(kText, 0.25f));
  c.line(x0 + w / 2 + 0.5f, kSmallTop, x0 + w / 2 + 0.5f, kSmallTop + kStrip, withAlphaF(kText, 0.25f));
  c.dashed(x0, kSmallTop + kStrip, x0 + w, kSmallTop, withAlphaF(kText, 0.3f), 1, 2, 3);
  std::vector<ImVec2> curve;
  for (int i = 0; i <= 160; i++) {
    const double in = -1 + 2.0 * i / 160;
    double out = saturation > 0 ? std::tanh(in * drive) / std::tanh(drive) : in;
    if (levels > 0) out = std::round(out * levels) / levels;
    curve.push_back(c.at(x0 + static_cast<float>((in + 1) / 2) * w, kSmallTop + kStrip / 2 - static_cast<float>(std::clamp(out, -1.0, 1.0)) * kStrip / 2));
  }
  c.dl->AddPolyline(curve.data(), static_cast<int>(curve.size()), kOrangeInk, 0, std::max(1.0f, 1.6f * c.scale));
  c.label("sat " + std::to_string(static_cast<int>(std::lround(saturation * 100))) + "%", x0 + 4, kSmallTop + 3, kOrangeInk, 0.9f);
}

// ---- the feedback: to its own side and across ----
void drawFeedback(const Canvas& c, const GraphNode& node, const NodeType& type) {
  const double own = valueOf(node, type, "feedback"), across = valueOf(node, type, "xfeedback");
  c.label("FEEDBACK", 8, 4, kText);
  const float cy = kSmallTop + kStrip * 0.58f, lx = kSmallW * 0.22f, rx = kSmallW * 0.78f, radius = 12;
  const auto arrow = [&](float x0, float y0, float x1, float y1, double amount) {
    const double a = std::fabs(amount);
    if (a < 0.005) return;
    const ImU32 colour = withAlphaF(amount < 0 ? kRed : kGreen, 0.9f);
    const float width = static_cast<float>(1 + a * 5);
    if (amount < 0) c.dashed(x0, y0, x1, y1, colour, width, 4, 3); else c.line(x0, y0, x1, y1, colour, width);
    const float angle = std::atan2(y1 - y0, x1 - x0);
    c.dl->AddTriangleFilled(c.at(x1, y1), c.at(x1 - 7 * std::cos(angle - 0.45f), y1 - 7 * std::sin(angle - 0.45f)), c.at(x1 - 7 * std::cos(angle + 0.45f), y1 - 7 * std::sin(angle + 0.45f)), colour);
  };
  const auto loop = [&](float centre, double amount) {
    const double a = std::fabs(amount);
    if (a < 0.005) return;
    const ImU32 colour = withAlphaF(amount < 0 ? kRed : kGreen, 0.9f);
    constexpr float pi = 3.14159265f;
    c.dl->PathArcTo(c.at(centre, cy - radius - 9), 10 * c.scale, pi * 0.85f, pi * 2.15f);
    c.dl->PathStroke(colour, 0, std::max(1.0f, static_cast<float>(1 + a * 5) * c.scale));
  };
  loop(lx, own); loop(rx, own);
  arrow(lx + radius + 2, cy - 5, rx - radius - 6, cy - 5, across);
  arrow(rx - radius - 2, cy + 6, lx + radius + 6, cy + 6, across);
  for (const auto& [centre, name] : {std::pair<float, const char*>{lx, "L"}, {rx, "R"}}) {
    c.dl->AddCircle(c.at(centre, cy), radius * c.scale, withAlphaF(kText, 0.8f), 0, std::max(1.0f, 1.2f * c.scale));
    c.label(name, centre - 3, cy - 4, kText, 0.9f);
  }
  const auto percent = [](double v) { return std::string(v < 0 ? "-" : "") + std::to_string(static_cast<int>(std::lround(std::fabs(v) * 100))) + "%"; };
  c.label("own " + percent(own), 8, kSmallTop + kStrip + 3, own < 0 ? kRed : kGreen, 0.9f);
  c.label("across " + percent(across), kSmallW - 8, kSmallTop + kStrip + 3, across < 0 ? kRed : kGreen, 0.9f, 1);
}

// ---- the pattern: its steps (the first is the highest bit, so the row reads as the binary number) ----
void drawPattern(const Canvas& c, const GraphNode& node, const NodeType& type, ResponseHandles& handles) {
  const double onset = valueOf(node, type, "onset");
  const long steps = std::lround(valueOf(node, type, "pattern"));
  const int length = std::clamp(static_cast<int>(std::lround(valueOf(node, type, "patlen"))), 1, 16), moves = static_cast<int>(std::lround(valueOf(node, type, "moves")));
  static const char* kMoves[] = {"new targets", "phase jump", "targets + phase"};
  c.label(std::string("PATTERN \xc2\xb7 ") + (onset > 0 || steps > 0 ? kMoves[std::clamp(moves, 0, 2)] : "off: random timer"), 8, 4, kText);
  const float width = (kPatternW - 16) / length;
  for (int k = 0; k < length; k++) {
    const float x = 8 + k * width;
    c.rect(x + 1.5f, kPatternTop + 0.5f, width - 3, kPatternSteps - 4, withAlphaF(kOrangeInk, 0.7f));
    if ((steps >> (length - 1 - k)) & 1) c.fill(x + 3, kPatternTop + 2, width - 6, kPatternSteps - 7, withAlphaF(kOrangeInk, 0.85f));
    for (const float part : {0.2f, 0.5f, 0.8f}) addHandle(handles, node, "@pogstep" + std::to_string(k), c.at(x + width * part, kPatternTop + kPatternSteps / 2), c.at(0, 0), ImVec2(1, 1), true);
  }
  std::string bits;
  for (int k = 0; k < length; k++) bits += ((steps >> (length - 1 - k)) & 1) ? '1' : '0';
  c.label((steps > 0 ? bits : std::string("off")) + " \xc2\xb7 click the steps", 8, kPatternTop + kPatternSteps + 1, kOrangeInk, 0.8f);
}

}  // namespace

void drawPogue(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 origin, float scale, const PictureTiming& timing, ResponseHandles& handles) {
  const History& history = updatedHistory(node, timing);
  const auto part = [&](float x) { return Canvas{dl, font, ImVec2(origin.x + x * scale, origin.y), scale}; };
  float x = 0;
  drawVoices(part(x), node, type, history, handles);
  x += kVoicesW + kGap;
  drawFilters(part(x), node, type, handles);
  x += kSmallW + kGap;
  drawShaper(part(x), node, type);
  x += kSmallW + kGap;
  drawFeedback(part(x), node, type);
  x += kSmallW + kGap;
  drawPattern(part(x), node, type, handles);
}

std::vector<std::pair<std::string, double>> dragPogueHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at) {
  const double fx = std::clamp(static_cast<double>((at.x - handle.gx) / handle.gw), 0.0, 1.0), fy = std::clamp(static_cast<double>((at.y - handle.gy) / handle.gh), 0.0, 1.0);
  const auto delay = [&](const char* name) { return std::vector<std::pair<std::string, double>>{{name, std::round(std::clamp((1 - fy) * axisMax(node, type), 0.5, 50.0) * 10) / 10}}; };
  if (handle.param == "@pogmin") return delay("mindelay");
  if (handle.param == "@pogmax") return delay("maxdelay");
  if (handle.param == "@pogonset") return {{"onset", std::round(sensitivityOf(std::exp((1 - fy) * std::log(kRatioMax))) * 100) / 100}};
  if (handle.param == "@pogfhp" || handle.param == "@pogflp") {
    const double f = std::pow(2.0, std::log2(20.0) + fx * (std::log2(20000.0) - std::log2(20.0)));
    return handle.param == "@pogfhp" ? std::vector<std::pair<std::string, double>>{{"hpcut", std::round(std::clamp(f, 10.0, 2000.0))}} : std::vector<std::pair<std::string, double>>{{"lpcut", std::round(std::clamp(f, 200.0, 20000.0))}};
  }
  if (handle.param.rfind("@pogstep", 0) == 0) {   // a step turned on or off
    const int length = std::clamp(static_cast<int>(std::lround(valueOf(node, type, "patlen"))), 1, 16), k = std::stoi(handle.param.substr(8));
    const long bits = std::lround(valueOf(node, type, "pattern"));
    return {{"pattern", static_cast<double>((bits ^ (1L << (length - 1 - std::clamp(k, 0, length - 1)))) & 0xffff)}};
  }
  return {};
}

}  // namespace wax::app
