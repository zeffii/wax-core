#include "app/space_pictures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "app/picture_canvas.h"
#include "app/theme.h"
#include "nodes/fft.h"
#include "nodes/filterpro_response.h"
#include "nodes/spacecore.h"
#include "nodes/specpan_core.h"

namespace wax::app {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kButtonHeight = 18;

double valueOf(const GraphNode& node, const NodeType& type, const std::string& name) {
  for (const ParamSpec& spec : type.params) if (spec.name == name) return node.param(name, spec.def);
  return 0;
}

double clampTo(const NodeType& type, const std::string& name, double v) {
  for (const ParamSpec& spec : type.params) if (spec.name == name) return std::min(spec.max, std::max(spec.min, v));
  return v;
}

const char* noteName(int midi) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  static char text[16];
  const int m = (midi % 12 + 12) % 12;
  std::snprintf(text, sizeof text, "%s%d", names[m], static_cast<int>(std::floor(midi / 12.0)) - 1);
  return text;
}

// The live data of a node: the last report's number, and when a report last came (for what goes stale).
std::string keyOf(const GraphNode& node, const PictureTiming& timing) { return timing.instrument + "/" + node.id; }
std::shared_ptr<SpectrumProbe> probeFor(const GraphNode& node, const PictureTiming& timing) {
  const std::shared_ptr<SpectrumProbe> probe = timing.probeOf ? timing.probeOf(timing.instrument, node.id) : nullptr;
  if (probe) probe->watch();
  return probe;
}

// a row of buttons from (x, y), wrapping at `right`; returns the y of the next row
float buttonRow(const Canvas& c, ResponseHandles& handles, const GraphNode& node, const std::vector<PictureButton>& buttons, float x, float y, float right, ImU32 accent, float* endX = nullptr) {
  float at = x;
  for (const PictureButton& button : buttons) {
    const float w = c.textWidth(button.text) + 14;
    if (at + w > right && at > x) { at = x; y += kButtonHeight + 4; }
    drawButton(c, handles, node, button, at, y, w, kButtonHeight, accent);
    at += w + 4;
  }
  if (endX) *endX = at;
  return y;
}

// ---- headpan ----
constexpr float kHeadW = 560, kHeadH = 280, kHeadCX = 150, kHeadCY = 130, kHeadR = 30, kHeadMax = 112;

float headRadius(double distance) { return kHeadR + 14 + static_cast<float>(std::clamp(std::log2(distance / 0.5) / std::log2(40.0), 0.0, 1.0)) * (kHeadMax - kHeadR - 14); }
double headDistance(double radius) { return 0.5 * std::pow(40.0, std::clamp((radius - kHeadR - 14) / (kHeadMax - kHeadR - 14), 0.0, 1.0)); }

void drawHeadpan(const Canvas& c, const GraphNode& node, const NodeType& type, ResponseHandles& handles) {
  const double angle = valueOf(node, type, "angle"), distance = valueOf(node, type, "distance");
  const space::Head h = space::headOf(angle, distance, valueOf(node, type, "head"), valueOf(node, type, "shadow"));
  const ImU32 ink = kText, blue = inkBlue(), pink = inkPink(), orange = inkOrange();
  for (const double m : {1.0, 3.0, 10.0}) {
    c.circle(kHeadCX, kHeadCY, headRadius(m), withAlphaF(ink, 0.18f));
    c.label(number(m, 0) + " m", kHeadCX + headRadius(m) * 0.71f + 2, kHeadCY - headRadius(m) * 0.71f - 12, ink, 0.5f);
  }
  c.circle(kHeadCX, kHeadCY, kHeadR, withAlphaF(ink, 0.8f), 2);   // the head, with a nose and two ears
  c.polyline({c.at(kHeadCX - 6, kHeadCY - kHeadR + 1), c.at(kHeadCX, kHeadCY - kHeadR - 9), c.at(kHeadCX + 6, kHeadCY - kHeadR + 1)}, withAlphaF(ink, 0.8f), 2);
  c.fill(kHeadCX - kHeadR - 6, kHeadCY - 6, 6, 12, blue);
  c.fill(kHeadCX + kHeadR, kHeadCY - 6, 6, 12, pink);
  const double a = angle * kPi / 180;   // the sound
  const float rr = headRadius(distance), sx = kHeadCX + static_cast<float>(std::sin(a)) * rr, sy = kHeadCY - static_cast<float>(std::cos(a)) * rr;
  c.line(kHeadCX, kHeadCY, sx, sy, withAlphaF(orange, 0.4f));
  c.dot(sx, sy, 8, orange);
  const auto ear = [&](float x, const char* name, ImU32 colour, bool far) {   // each ear: late, loud, dull
    const double late = far ? h.itd * 1e6 : 0, gain = h.gain * (far ? h.farGain : 1), cut = std::min(far ? h.farCut : 20000.0, h.rearCut);
    char text[48];
    c.label(name, x, 40, colour, 1);
    std::snprintf(text, sizeof text, "%.0f us late", late); c.label(text, x, 62, ink, 0.8f);
    std::snprintf(text, sizeof text, "%.1f dB", 20 * std::log10(gain)); c.label(text, x, 80, ink, 0.8f);
    c.label(std::string("low-pass ") + (cut >= 19000 ? std::string("open") : number(cut / 1000) + " kHz"), x, 98, ink, 0.8f);
    c.fill(x, 130, 100, 8, withAlphaF(ink, 0.25f));
    c.fill(x, 130, 100 * static_cast<float>(std::clamp(gain, 0.0, 1.0)), 8, colour);
    c.fill(x, 150, 100, 8, withAlphaF(ink, 0.25f));
    c.fill(x, 150, 100 * static_cast<float>(std::clamp(std::log2(cut / 1000) / std::log2(20.0), 0.02, 1.0)), 8, colour);
  };
  ear(340, "left ear", blue, h.farLeft);
  ear(460, "right ear", pink, !h.farLeft);
  c.label("level", 340, 119, ink, 0.5f);
  c.label("brightness", 340, 139, ink, 0.5f);
  const std::string side = std::fabs(angle) <= 180 ? (angle > 0 ? number(std::round(angle), 0) + " deg right" : angle < 0 ? number(std::round(-angle), 0) + " deg left" : "ahead") : "";
  c.label(side + ", " + number(distance) + " m   time difference " + number(h.itd * 1e6, 0) + " us   (drag the orange dot: round the head sets the angle, in and out the distance)", 8, kHeadH - 14, ink, 0.6f);
  addHandle(handles, node, "@sp.head", c.at(sx, sy), c.at(kHeadCX, kHeadCY), ImVec2(c.scale, 0));
}

// ---- ringmod ----
constexpr float kRingW = 560, kRingH = 210, kRingPL = 14, kRingPR = 14;
constexpr double kRingFMin = 30, kRingFMax = 12000;
constexpr int kRingHarmonics = 6;

struct RingLive { double f0 = 0, fc = 0; float number = -1; };
std::map<std::string, RingLive>& ringLive() { static std::map<std::string, RingLive> live; return live; }
std::map<std::string, double>& ringPitch() { static std::map<std::string, double> pitch; return pitch; }   // by node id only, for the drag

float ringX(double f) { return kRingPL + static_cast<float>((std::log2(std::clamp(f, kRingFMin, kRingFMax)) - std::log2(kRingFMin)) / (std::log2(kRingFMax) - std::log2(kRingFMin))) * (kRingW - kRingPL - kRingPR); }

void drawRingmod(const Canvas& c, const GraphNode& node, const NodeType& type, const PictureTiming& timing, ResponseHandles& handles) {
  RingLive& live = ringLive()[keyOf(node, timing)];
  if (const std::shared_ptr<SpectrumProbe> probe = probeFor(node, timing)) {
    std::vector<float> in, out;
    if (probe->read(in, out) && in.size() >= 3 && in[0] != live.number) { live.number = in[0]; live.f0 = in[1]; live.fc = in[2]; }
  }
  ringPitch()[node.id] = live.f0;
  const bool tracked = valueOf(node, type, "track") >= 0.5;
  const double f0 = live.f0 > 0 ? live.f0 : 220;
  const double fc = live.fc > 0 ? live.fc : tracked ? f0 * valueOf(node, type, "ratio") + valueOf(node, type, "offset") : valueOf(node, type, "freq");
  const ImU32 ink = kText, orange = inkOrange(), blue = inkBlue(), pink = inkPink();
  for (const int f : {50, 100, 200, 500, 1000, 2000, 5000, 10000}) {
    c.line(ringX(f) + 0.5f, 10, ringX(f) + 0.5f, kRingH - 24, withAlphaF(ink, 0.15f));
    c.label(f >= 1000 ? std::to_string(f / 1000) + "k" : std::to_string(f), ringX(f) - 8, kRingH - 18, ink, 0.55f);
  }
  const float base = kRingH - 28, top = 24;
  const auto stick = [&](double f, double level, ImU32 colour, float width, float alpha, bool dashed = false) {
    if (f < kRingFMin || f > kRingFMax) return;
    if (dashed) c.dashed(ringX(f), base, ringX(f), base - static_cast<float>(level) * (base - top), withAlphaF(colour, alpha), width, 4, 3);
    else c.line(ringX(f), base, ringX(f), base - static_cast<float>(level) * (base - top), withAlphaF(colour, alpha), width);
  };
  for (int n = 1; n <= kRingHarmonics; n++) stick(n * f0, 1.0 / n, ink, 3, 0.7f);   // the sound's partials, falling with 1/n
  for (int n = 1; n <= kRingHarmonics; n++) {   // the sidebands of each: carrier minus the partial (pink) and plus it (blue)
    stick(std::fabs(n * f0 - fc), 0.9 / n * 0.8, pink, 2, 1);
    stick(n * f0 + fc, 0.9 / n * 0.8, blue, 2, 1);
  }
  stick(fc, 1, orange, 1.5f, 0.9f, true);
  c.label("carrier " + number(std::round(fc), 0) + " Hz", std::clamp(ringX(fc) + 4, 4.0f, kRingW - 110), 6, orange, 1);
  c.label(std::string("pitch ") + (live.f0 > 0 ? number(std::round(f0), 0) + " Hz" : "not heard (showing 220)"), kRingW - 170, 6, ink, 0.6f);
  addRegionHandles(handles, node, "@sp.ring", c, kRingPL, 10, kRingW - kRingPR, kRingH - 24, c.at(kRingPL, 0), ImVec2((kRingW - kRingPL - kRingPR) * c.scale, 1));
  // the buttons: follow pitch, the carrier's wave
  std::vector<PictureButton> buttons = {{"follow pitch", "@flip:track", tracked}};
  const char* waves[] = {"sine", "triangle", "square", "saw"};
  for (int i = 0; i < 4; i++) buttons.push_back({waves[i], "@set:wave=" + std::to_string(i), std::lround(valueOf(node, type, "wave")) == i});
  float end = 8;
  buttonRow(c, handles, node, buttons, 8, kRingH + 4, 100000, orange, &end);
  c.label(tracked ? "ratio " + number(valueOf(node, type, "ratio"), 2) : "fixed", end + 6, kRingH + 4 + 4, ink, 0.6f);
}

// ---- notepan ----
constexpr float kPanW = 560, kPanH = 330, kPanCX = 150, kPanCY = 150, kPanR = 110, kPanSY = 296;
struct PanVoice { int note = -1; double angle = 0; bool held = false; };
struct PanLive { std::vector<PanVoice> voices; float number = -1; double at = 0; };
std::map<std::string, PanLive>& panLive() { static std::map<std::string, PanLive> live; return live; }

void drawNotepan(const Canvas& c, const GraphNode& node, const NodeType& type, const PictureTiming& timing, ResponseHandles& handles) {
  PanLive& live = panLive()[keyOf(node, timing)];
  if (const std::shared_ptr<SpectrumProbe> probe = probeFor(node, timing)) {
    std::vector<float> in, out;
    if (probe->read(in, out) && in.size() >= 2 && in[0] != live.number && in.size() >= 2 + 4 * static_cast<std::size_t>(in[1])) {
      live.number = in[0];
      live.at = ImGui::GetTime();
      live.voices.clear();
      for (std::size_t i = 0; i < static_cast<std::size_t>(in[1]); i++) live.voices.push_back({static_cast<int>(in[2 + 4 * i + 1]), in[2 + 4 * i + 2], in[2 + 4 * i + 3] > 0.5f});
    }
  }
  if (ImGui::GetTime() - live.at > 0.4) live.voices.clear();   // nothing has been heard for a while: the notes have stopped
  const double range = valueOf(node, type, "range"), centre = valueOf(node, type, "centre"), rate = valueOf(node, type, "rate"), amp = valueOf(node, type, "amp");
  const int shape = static_cast<int>(std::lround(valueOf(node, type, "shape")));
  const ImU32 ink = kText, orange = inkOrange(), blue = inkBlue(), pink = inkPink();
  c.circle(kPanCX, kPanCY, kPanR, withAlphaF(ink, 0.35f));   // the circle, the listener and the front
  c.line(kPanCX - kPanR, kPanCY, kPanCX + kPanR, kPanCY, withAlphaF(ink, 0.15f));
  c.line(kPanCX, kPanCY - kPanR, kPanCX, kPanCY + kPanR, withAlphaF(ink, 0.15f));
  c.label("front", kPanCX - 12, kPanCY - kPanR - 14, ink, 0.6f); c.label("back", kPanCX - 11, kPanCY + kPanR + 4, ink, 0.6f);
  c.label("L", kPanCX - kPanR - 14, kPanCY - 5, ink, 0.6f); c.label("R", kPanCX + kPanR + 6, kPanCY - 5, ink, 0.6f);
  c.circle(kPanCX, kPanCY, 9, withAlphaF(ink, 0.8f));
  c.polyline({c.at(kPanCX - 4, kPanCY - 8), c.at(kPanCX, kPanCY - 14), c.at(kPanCX + 4, kPanCY - 8)}, withAlphaF(ink, 0.8f), 1);
  // the field the notes are put in: from -range to +range round the centre, in front
  const float half = static_cast<float>(kPi / 2), a0 = static_cast<float>(std::clamp(centre - range, -1.0, 1.0)) * half, a1 = static_cast<float>(std::clamp(centre + range, -1.0, 1.0)) * half;
  c.dl->PathLineTo(c.at(kPanCX, kPanCY));
  c.dl->PathArcTo(c.at(kPanCX, kPanCY), kPanR * c.scale, a0 - half, a1 - half);
  c.dl->PathFillConcave(withAlphaF(orange, 0.14f));
  c.arc(kPanCX, kPanCY, kPanR, a0 - half, a1 - half, withAlphaF(orange, 0.7f));
  if (std::fabs(rate) > 0.001) {   // how far the rotation reaches
    if (shape == 2) c.circle(kPanCX, kPanCY, kPanR + 10, withAlphaF(pink, 0.6f));
    else c.arc(kPanCX, kPanCY, kPanR + 10, a0 - half - static_cast<float>(amp * kPi), a1 - half + static_cast<float>(amp * kPi), withAlphaF(pink, 0.6f));
  }
  std::vector<PanVoice> dots = live.voices;
  for (const PanVoice& v : dots) {
    const float back = static_cast<float>(std::max(0.0, -std::cos(v.angle))), x = kPanCX + static_cast<float>(std::sin(v.angle)) * kPanR, y = kPanCY - static_cast<float>(std::cos(v.angle)) * kPanR;
    c.dot(x, y, 7 - 2 * back, withAlphaF(blue, (v.held ? 1.0f : 0.55f) * (1 - 0.6f * back)));
    if (v.note >= 0) c.label(noteName(v.note), x + 9, y - 5, ink, 0.8f * (1 - 0.6f * back));
  }
  c.line(20, kPanSY, kPanW - 20, kPanSY, withAlphaF(ink, 0.3f));   // the same, as the speakers get it
  c.label("L", 8, kPanSY - 5, ink, 0.5f); c.label("R", kPanW - 12, kPanSY - 5, ink, 0.5f); c.label("C", kPanW / 2 - 3, kPanSY + 8, ink, 0.5f);
  for (const PanVoice& v : dots) c.dot(kPanW / 2 + static_cast<float>(std::sin(v.angle)) * (kPanW / 2 - 30), kPanSY, 5, withAlphaF(blue, 0.9f * (v.held ? 1.0f : 0.55f)));
  c.label(dots.empty() ? "play a note" : "sounding", 330, 24, ink, 0.6f);   // the list on the right
  std::sort(dots.begin(), dots.end(), [](const PanVoice& a, const PanVoice& b) { return a.note < b.note; });
  for (std::size_t i = 0; i < std::min<std::size_t>(10, dots.size()); i++) {
    const int deg = static_cast<int>(std::lround(dots[i].angle * 180 / kPi)), wrapped = ((deg + 180) % 360 + 360) % 360 - 180;
    c.label(std::string(noteName(dots[i].note)) + "  " + (std::abs(wrapped) <= 90 ? "front " : "back ") + (wrapped == 0 ? std::string("centre") : std::to_string(std::abs(wrapped)) + " deg " + (wrapped > 0 ? "right" : "left")), 330, 44 + static_cast<float>(i) * 16, ink, dots[i].held ? 0.9f : 0.5f);
  }
  addRegionHandles(handles, node, "@sp.npcentre", c, kPanCX - kPanR - 14, kPanCY - kPanR - 14, kPanCX + kPanR + 14, kPanCY + kPanR + 14, c.at(kPanCX, kPanCY), ImVec2(kPanR * c.scale, 1));
  // the buttons: the style of placing, the shape of the rotation
  static const char* styles[] = {"keyboard", "fan", "alternate", "random", "cluster", "mirror"};
  static const char* shapes[] = {"swing", "triangle", "orbit"};
  std::vector<PictureButton> row1, row2;
  for (int i = 0; i < 6; i++) row1.push_back({styles[i], "@set:style=" + std::to_string(i), std::lround(valueOf(node, type, "style")) == i});
  for (int i = 0; i < 3; i++) row2.push_back({shapes[i], "@set:shape=" + std::to_string(i), shape == i});
  const float y = buttonRow(c, handles, node, row1, 8, kPanH + 4, 100000, orange);
  float end = 8;
  buttonRow(c, handles, node, row2, 8, y + kButtonHeight + 4, 100000, orange, &end);
  c.label(std::string(std::fabs(rate) < 0.001 ? "still" : number(rate, 2) + " Hz") + (shape == 2 ? "  (orbit: amp not used)" : ""), end + 6, y + kButtonHeight + 8, ink, 0.6f);
}

// ---- sympathy ----
constexpr float kSymW = 560, kSymPL = 16, kSymPR = 16, kSymBars = 38, kSymPH = 190, kSymH = kSymPH + kSymBars;
struct SymLive { std::vector<float> energy, notes, left, right; float number = -1; };
std::map<std::string, SymLive>& symLive() { static std::map<std::string, SymLive> live; return live; }
constexpr double kSymLo = 4.906890595608519, kSymHi = 11.965784284662087;   // log2(30), log2(4000)

float symX(double hz) { return kSymPL + static_cast<float>((std::log2(std::clamp(hz, 30.0, 4000.0)) - kSymLo) / (kSymHi - kSymLo)) * (kSymW - kSymPL - kSymPR); }

void drawSympathy(const Canvas& c, const GraphNode& node, const NodeType& type, const PictureTiming& timing, ResponseHandles& handles) {
  SymLive& live = symLive()[keyOf(node, timing)];
  if (const std::shared_ptr<SpectrumProbe> probe = probeFor(node, timing)) {
    std::vector<float> in, out;
    if (probe->read(in, out) && in.size() >= 2 && in[0] != live.number) {
      const std::size_t n = static_cast<std::size_t>(in[1]);
      if (in.size() >= 2 + 4 * n) {
        live.number = in[0];
        const auto part = [&](std::size_t k) { return std::vector<float>(in.begin() + static_cast<std::ptrdiff_t>(2 + k * n), in.begin() + static_cast<std::ptrdiff_t>(2 + (k + 1) * n)); };
        live.notes = part(0); live.energy = part(1); live.left = part(2); live.right = part(3);
      }
    }
  }
  const bool follow = valueOf(node, type, "follow") >= 0.5;
  std::vector<int> notes;
  if (follow) for (const float m : live.notes) notes.push_back(static_cast<int>(m));
  else notes = space::stringsOf(valueOf(node, type, "root"), valueOf(node, type, "chord"), valueOf(node, type, "strings"));
  const ImU32 ink = kText, orange = inkOrange(), blue = inkBlue();
  const auto energyOf = [&](std::size_t i) { return i < live.energy.size() ? live.energy[i] : 0.0f; };
  for (int m = 24; m <= 108; m += 12) {
    const float x = symX(space::midiHz(m));
    c.line(x + 0.5f, 8, x + 0.5f, kSymPH - 28, withAlphaF(ink, 0.12f));
    c.label(noteName(m), x - 8, kSymPH - 18, ink, 0.5f);
  }
  const float base = kSymPH - 30, top = 28;
  for (std::size_t i = 0; i < notes.size(); i++) {
    const float x = symX(space::midiHz(notes[i])), e = std::clamp(energyOf(i) * 3, 0.0f, 1.0f);
    c.line(x, base, x, top, withAlphaF(i == 0 ? orange : ink, 0.35f + (i == 0 ? 0.4f : 0.0f)), 2);
    if (e > 0.01f) c.line(x, base, x, base - (base - top) * e, withAlphaF(blue, 0.9f), 4 + 6 * e);
    c.label(noteName(notes[i]), x - 8, top - 14, i == 0 ? orange : ink, 1);
  }
  for (int side = 0; side < 2; side++) {   // the sum: two bars, left above right, a segment for each string
    const float y = kSymPH + 4 + static_cast<float>(side) * 15;
    float x = kSymPL + 12;
    c.label(side ? "R" : "L", kSymPL, y - 1, ink, 0.6f);
    for (std::size_t i = 0; i < notes.size(); i++) {
      const std::vector<float>& pans = side ? live.right : live.left;
      const float w = std::clamp(energyOf(i) * 3, 0.0f, 1.0f) * (i < pans.size() ? pans[i] : 0.7071f) * 26;
      if (w < 0.5f) continue;
      c.fill(x, y, w, 10, withAlphaF(hslColour(static_cast<float>(200 + (notes[i] - 30) * 2.2), 0.65f, 0.52f), 0.9f));
      x += w;
    }
  }
  if (!follow) addRegionHandles(handles, node, "@sp.symroot", c, kSymPL, 8, kSymW - kSymPR, kSymPH - 22, c.at(kSymPL, 0), ImVec2((kSymW - kSymPL - kSymPR) * c.scale, 1));
  // the buttons: follow notes, the chords, the root's steps
  std::vector<PictureButton> buttons = {{"follow notes", "@flip:follow", follow}};
  const std::vector<const char*>& names = space::chordNames();
  for (std::size_t i = 0; i < names.size(); i++) buttons.push_back({names[i], "@set:chord=" + std::to_string(i), !follow && static_cast<std::size_t>(std::lround(valueOf(node, type, "chord"))) == i});
  buttons.push_back({"-", "@nudge:root=-1", false}); buttons.push_back({"+", "@nudge:root=1", false});
  buttons.push_back({"-8", "@nudge:root=-12", false}); buttons.push_back({"+8", "@nudge:root=12", false});
  float end = 8;
  const float y = buttonRow(c, handles, node, buttons, 8, kSymH + 4, kSymW - 8, orange, &end);
  const double ring = valueOf(node, type, "ringtime") / 1000;
  c.label(follow ? std::to_string(notes.size()) + " strings, on the notes played   rings " + number(ring) + " s" + (notes.empty() ? "   (play a note)" : "")
                 : std::string("root ") + noteName(static_cast<int>(valueOf(node, type, "root"))) + "   " + std::to_string(notes.size()) + " strings   rings " + number(ring) + " s", end + 6, y + 4, ink, 0.6f);
}

// ---- specpan ----
constexpr float kSpecW = 360, kSpecH = 150, kSpecPL = 22, kSpecPR = 8, kSpecPT = 14, kSpecPB = 18;

void drawSpecpan(const Canvas& c, const GraphNode& node, const NodeType& type) {
  const int shape = static_cast<int>(std::lround(valueOf(node, type, "shape")));
  const double spread = valueOf(node, type, "spread"), rotate = valueOf(node, type, "rotate"), lowMono = valueOf(node, type, "lowMono");
  std::array<double, specpan::kBands> custom{}, pans{};
  for (int k = 0; k < specpan::kBands; k++) custom[static_cast<std::size_t>(k)] = valueOf(node, type, "b" + std::to_string(k + 1));
  specpan::bandPans(shape, rotate, custom.data(), pans.data());
  const float pw = kSpecW - kSpecPL - kSpecPR, ph = kSpecH - kSpecPT - kSpecPB, mid = kSpecPT + ph / 2;
  const auto xOf = [&](double f) { return kSpecPL + static_cast<float>(std::log(std::clamp(f, specpan::kFLow / 7.5, 20000.0) / 20.0) / std::log(20000.0 / 20.0)) * pw; };
  std::vector<double> edges = {20.0};
  for (const double cut : specpan::crossovers()) edges.push_back(cut);
  edges.push_back(20000.0);
  const ImU32 ink = kText;
  for (const int f : {50, 100, 200, 500, 1000, 2000, 5000, 10000}) {   // frequency grid
    const float x = std::round(xOf(f)) + 0.5f;
    c.line(x, kSpecPT, x, kSpecPT + ph, withAlphaF(ink, 0.13f));
    c.label(f >= 1000 ? std::to_string(f / 1000) + "k" : std::to_string(f), x - 8, kSpecH - kSpecPB + 4, ink, 0.55f);
  }
  c.label("L", 6, kSpecPT - 2, ink, 0.55f); c.label("R", 6, kSpecPT + ph - 9, ink, 0.55f);
  c.fill(kSpecPL, std::round(mid), pw, 1, withAlphaF(ink, 0.35f));
  for (int k = 0; k < specpan::kBands; k++) {   // one bar per band; up is left, as in the tap view
    const float x0 = xOf(std::max(edges[static_cast<std::size_t>(k)], lowMono)), x1 = xOf(edges[static_cast<std::size_t>(k) + 1]);
    if (x1 - x0 < 1) continue;
    const float p = static_cast<float>(std::clamp(pans[static_cast<std::size_t>(k)], -1.0, 1.0) * spread), h = p * (ph / 2), f = (static_cast<float>(pans[static_cast<std::size_t>(k)]) + 1) / 2;
    c.fill(x0 + 1, h < 0 ? mid + h : mid, std::max(1.0f, x1 - x0 - 2), std::max(1.0f, std::fabs(h)), IM_COL32(static_cast<int>(60 + 180 * f), 150, static_cast<int>(230 - 180 * f), 217));
  }
  const float xm = xOf(lowMono);   // the mono region
  c.fill(kSpecPL, kSpecPT, xm - kSpecPL, ph, withAlphaF(ink, 0.12f));
  c.label("mono", kSpecPL + 3, kSpecPT + 2, ink, 0.6f);
}

// ---- filterpro ----
constexpr float kFilterW = 560, kFilterPL = 40, kFilterPR = 20, kFilterTop = 22, kFilterPlot = 190, kFilterH = kFilterTop + kFilterPlot + 34 + 16;
constexpr double kFilterDbTop = 24, kFilterDbBottom = -36;
constexpr int kFilterFrame = 2048, kFilterPoints = 240;
const char* kFilterModes[] = {"lowpass", "bandpass", "highpass", "notch", "peak", "allpass", "morph", "lowpass 24", "ladder", "vowel", "comb +", "comb -"};

struct FilterLive { std::vector<float> input = std::vector<float>(kFilterPoints, 0.0f), output = std::vector<float>(kFilterPoints, 0.0f); bool live = false; double checksum = -1; };
std::map<std::string, FilterLive>& filterLive() { static std::map<std::string, FilterLive> live; return live; }

void spectrumOf(const std::vector<float>& frame, std::vector<float>& shown) {
  std::vector<double> re(kFilterFrame), im(kFilterFrame, 0.0);
  for (int i = 0; i < kFilterFrame; i++) re[static_cast<std::size_t>(i)] = frame[static_cast<std::size_t>(i)] * (0.5 - 0.5 * std::cos(2 * kPi * i / kFilterFrame));
  fftPlan(kFilterFrame).run(re.data(), im.data());
  std::vector<float> points(kFilterPoints);
  filterpro::spectrumPoints(re.data(), im.data(), kFilterFrame, 48000, points);
  for (std::size_t i = 0; i < shown.size(); i++) shown[i] = std::max(points[i], shown[i] * 0.85f);   // they fall a little slower than they rise
}

std::string hertzLong(double v) { return v >= 1000 ? number(v / 1000, v >= 10000 ? 0 : 2) + " kHz" : number(std::round(v), 0) + " Hz"; }

void drawFilterpro(const Canvas& c, const GraphNode& node, const NodeType& type, const PictureTiming& timing, ResponseHandles& handles) {
  FilterLive& live = filterLive()[keyOf(node, timing)];
  if (const std::shared_ptr<SpectrumProbe> probe = probeFor(node, timing)) {
    std::vector<float> in, out;
    if (probe->read(in, out) && in.size() == kFilterFrame && out.size() == kFilterFrame) {
      double sum = 0;
      for (int i = 0; i < kFilterFrame; i += 16) sum += in[static_cast<std::size_t>(i)] * (i + 1) + out[static_cast<std::size_t>(i)] * (i + 3);
      if (sum != live.checksum) { live.checksum = sum; spectrumOf(in, live.input); spectrumOf(out, live.output); live.live = true; }
    }
  }
  const filterpro::Get get = [&](const char* name) { return valueOf(node, type, name); };
  const int mode = static_cast<int>(std::lround(get("mode")));
  const double fc = std::clamp(get("cutoff"), 20.0, 18000.0), lo = std::log2(filterpro::kFLow), hi = std::log2(filterpro::kFHigh);
  const float pw = kFilterW - kFilterPL - kFilterPR;
  const auto X = [&](double f) { return kFilterPL + static_cast<float>((std::log2(f) - lo) / (hi - lo)) * pw; };
  const auto Y = [&](double db) { return kFilterTop + kFilterPlot - static_cast<float>((std::clamp(db, kFilterDbBottom, kFilterDbTop) - kFilterDbBottom) / (kFilterDbTop - kFilterDbBottom)) * kFilterPlot; };
  const ImU32 ink = kText, orange = inkOrange(), green = hslColour(165, 0.55f, 0.52f), amber = hslColour(40, 0.95f, 0.62f);
  const std::pair<int, const char*> labels[] = {{50, "50"}, {100, "100"}, {200, "200"}, {500, "500"}, {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}};
  for (const auto& [f, text] : labels) {
    c.line(std::round(X(f)) + 0.5f, kFilterTop, std::round(X(f)) + 0.5f, kFilterTop + kFilterPlot, withAlphaF(ink, 0.13f));
    c.label(text, X(f), kFilterTop + kFilterPlot + 4, ink, 0.6f, 2);
  }
  c.line(kFilterPL, kFilterTop + kFilterPlot + 0.5f, kFilterW - kFilterPR, kFilterTop + kFilterPlot + 0.5f, withAlphaF(ink, 0.35f));
  c.label("INPUT", kFilterPL, 6, ink, 0.6f); c.label("OUTPUT", kFilterPL + 46, 6, orange, 0.6f); c.label("FILTER", kFilterPL + 100, 6, green, 0.6f);
  const auto spectrum = [&](const std::vector<float>& values, ImU32 colour, float fill) {   // along the bottom of the plot, 0 dB at the top and -90 dB at the bottom
    const float y0 = kFilterTop + kFilterPlot;
    std::vector<ImVec2> area = {c.at(kFilterPL, y0)}, curve;
    for (std::size_t i = 0; i < values.size(); i++) curve.push_back(c.at(kFilterPL + (static_cast<float>(i) + 0.5f) / values.size() * pw, y0 - values[i] * (kFilterPlot - 6)));
    area.insert(area.end(), curve.begin(), curve.end());
    area.push_back(c.at(kFilterW - kFilterPR, y0));
    c.dl->AddConcavePolyFilled(area.data(), static_cast<int>(area.size()), withAlphaF(colour, fill));
    c.polyline(curve, colour, 1.1f);
  };
  spectrum(live.input, ink, 0.16f);
  spectrum(live.output, orange, 0.3f);
  if (!live.live) c.label("play something through it to see the sound", kFilterPL + pw / 2, kFilterTop + kFilterPlot / 2 + 20, ink, 0.45f, 2);
  c.dashed(kFilterPL, std::round(Y(0)) + 0.5f, kFilterW - kFilterPR, std::round(Y(0)) + 0.5f, withAlphaF(green, 0.45f), 1, 3, 3);   // the curve, on its own scale
  c.label("+" + number(kFilterDbTop, 0), kFilterPL - 4, Y(kFilterDbTop) - 1, green, 0.8f, 1);
  c.label("0 dB", kFilterPL - 4, Y(0) - 4, green, 0.8f, 1);
  c.label(number(kFilterDbBottom, 0), kFilterPL - 4, Y(kFilterDbBottom) - 8, green, 0.8f, 1);
  std::vector<double> freqs, gains;
  std::vector<ImVec2> curve;
  for (int i = 0; i <= 300; i++) {
    const double f = filterpro::kFLow * std::pow(filterpro::kFHigh / filterpro::kFLow, i / 300.0);
    freqs.push_back(f); gains.push_back(filterpro::responseDb(f, get));
    curve.push_back(c.at(X(f), Y(gains.back())));
  }
  c.polyline(curve, green, 1.7f);
  const auto mark = [&](double f, const std::string& label, ImU32 colour, bool dotToo, int row) {   // what the voicing adds
    const float x = X(f);
    c.dashed(std::round(x) + 0.5f, kFilterTop + 12 + row * 10, std::round(x) + 0.5f, kFilterTop + kFilterPlot, withAlphaF(colour, 0.7f), 1, 3, 3);
    c.label(label, std::clamp(x, kFilterPL + 24, kFilterW - kFilterPR - 24), kFilterTop + 1 + row * 10, colour, 1, 2);
    if (dotToo) c.dot(x, Y(filterpro::responseDb(f, get)), 4, colour);
  };
  std::string note;
  if (mode == 9) {
    const std::vector<double> formants = filterpro::formantsAt(get("vowel"), fc);
    for (std::size_t q = 0; q < formants.size(); q++) mark(formants[q], "F" + std::to_string(q + 1) + " " + hertzLong(formants[q]), green, true, static_cast<int>(q % 2));
    note = std::string("vowel ") + "aeiou"[static_cast<int>(std::clamp(std::lround(get("vowel")), 0L, 4L))] + " - formants at " + hertzLong(formants[0]) + ", " + hertzLong(formants[1]) + ", " + hertzLong(formants[2]) + " (they follow the cutoff, 1 kHz is the written pitch)";
  } else if (mode >= 10) {
    for (int n = 1; n * fc < filterpro::kFHigh && n <= 16; n++) {
      if (n <= 5) mark(n * fc, std::to_string(n) + "x", n > 1 ? green : amber, false, 0);
      else c.line(std::round(X(n * fc)) + 0.5f, kFilterTop + 12, std::round(X(n * fc)) + 0.5f, kFilterTop + kFilterPlot, withAlphaF(green, 0.25f));
    }
    note = std::string(kFilterModes[mode]) + " - teeth at multiples of " + hertzLong(fc) + " (" + (mode == 10 ? "peaks" : "notches, then peaks") + " on the whole multiples)";
  } else {
    mark(fc, "cutoff " + hertzLong(fc), amber, true, 0);
    std::size_t topAt = 0;
    for (std::size_t i = 1; i < gains.size(); i++) if (gains[i] > gains[topAt]) topAt = i;
    note = std::string(kFilterModes[std::clamp(mode, 0, 11)]) + " - highest point " + (gains[topAt] >= 0 ? "+" : "-") + number(std::fabs(gains[topAt])) + " dB at " + hertzLong(freqs[topAt]);
  }
  if (get("mix") < 1) note += " - mix " + std::to_string(static_cast<int>(std::lround(get("mix") * 100))) + "%";
  c.label(note, 8, kFilterH - 14, ink, 0.6f);
  // the cutoff's dot is dragged along the axis for the cutoff and up and down for the resonance: grabbed anywhere on its vertical line
  for (float y = kFilterTop + 4; y <= kFilterTop + kFilterPlot; y += 12) addHandle(handles, node, "@sp.fpro", c.at(X(fc), y), c.at(kFilterPL, kFilterTop), ImVec2(pw * c.scale, kFilterPlot * c.scale));
}

}  // namespace

PictureSize spacePictureSize(const std::string& nodeType) {
  if (nodeType == "headpan") return {kHeadW, kHeadH};
  if (nodeType == "ringmod") return {kRingW, kRingH + 4 + kButtonHeight + 6};
  if (nodeType == "notepan") return {kPanW, kPanH + 4 + 2 * kButtonHeight + 8 + 6};
  if (nodeType == "sympathy") return {kSymW, kSymH + 4 + 2 * kButtonHeight + 8 + 6};
  if (nodeType == "specpan") return {kSpecW, kSpecH};
  if (nodeType == "filterpro") return {kFilterW, kFilterH};
  return {};
}

void drawSpacePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 origin, float scale, const PictureTiming& timing, ResponseHandles& handles) {
  const Canvas canvas{dl, font, origin, scale};
  if (node.type == "headpan") drawHeadpan(canvas, node, type, handles);
  else if (node.type == "ringmod") drawRingmod(canvas, node, type, timing, handles);
  else if (node.type == "notepan") drawNotepan(canvas, node, type, timing, handles);
  else if (node.type == "sympathy") drawSympathy(canvas, node, type, timing, handles);
  else if (node.type == "specpan") drawSpecpan(canvas, node, type);
  else if (node.type == "filterpro") drawFilterpro(canvas, node, type, timing, handles);
}

std::vector<std::pair<std::string, double>> dragSpaceHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at) {
  const double fx = std::clamp(static_cast<double>((at.x - handle.gx) / std::max(1.0f, handle.gw)), 0.0, 1.0);
  if (handle.param == "@sp.head") {   // round the head sets the angle, in and out the distance (gx, gy: the head's centre; gw: pixels per web pixel)
    const double x = (at.x - handle.gx) / handle.gw, y = (at.y - handle.gy) / handle.gw;
    double degrees = std::atan2(x, -y) * 180 / kPi;
    if (std::fabs(degrees) < 3) degrees = 0;
    return {{"angle", clampTo(type, "angle", std::round(degrees))}, {"distance", clampTo(type, "distance", std::round(headDistance(std::hypot(x, y)) * 10) / 10)}};
  }
  if (handle.param == "@sp.ring") {   // the carrier: tracked it sets the ratio to the pitch, fixed it sets the frequency
    const double f = std::pow(2.0, std::log2(kRingFMin) + fx * (std::log2(kRingFMax) - std::log2(kRingFMin)));
    if (valueOf(node, type, "track") >= 0.5) {
      const auto pitch = ringPitch().find(node.id);
      const double f0 = pitch != ringPitch().end() && pitch->second > 0 ? pitch->second : 220;
      return {{"ratio", clampTo(type, "ratio", std::round(std::clamp((f - valueOf(node, type, "offset")) / f0, 0.25, 8.0) * 100) / 100)}};
    }
    return {{"freq", clampTo(type, "freq", std::round(std::clamp(f, 0.1, 5000.0) * 10) / 10)}};
  }
  if (handle.param == "@sp.npcentre") return {{"centre", clampTo(type, "centre", std::round(std::clamp((at.x - handle.gx) / handle.gw, -1.0f, 1.0f) * 100) / 100)}};
  if (handle.param == "@sp.symroot") {
    const double hz = std::pow(2.0, kSymLo + fx * (kSymHi - kSymLo));
    return {{"root", clampTo(type, "root", std::clamp(std::round(69 + 12 * std::log2(hz / 440)), 24.0, 84.0))}};
  }
  if (handle.param == "@sp.fpro") {   // the cutoff along the axis; the resonance by how far the mouse has moved up or down since the grab
    static std::map<std::string, std::pair<float, double>> grabs;   // by node: where the grab began (y) and the resonance then
    static int lastFrame = -10;
    std::pair<float, double>& grab = grabs[node.id];
    if (ImGui::GetFrameCount() - lastFrame > 1) grab = {at.y, valueOf(node, type, "resonance")};
    lastFrame = ImGui::GetFrameCount();
    const double f = std::pow(2.0, std::log2(filterpro::kFLow) + fx * (std::log2(filterpro::kFHigh) - std::log2(filterpro::kFLow)));
    return {{"cutoff", clampTo(type, "cutoff", std::round(std::clamp(f, 20.0, 18000.0)))}, {"resonance", clampTo(type, "resonance", std::round(std::clamp(grab.second + (grab.first - at.y) / handle.gh, 0.0, 1.0) * 100) / 100)}};
  }
  return {};
}

}  // namespace wax::app
