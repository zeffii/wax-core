#include "app/node_look.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace wax::app {

namespace {

struct GlyphStroke { int first, count; int fill, closed; float alpha; };   // points are x,y pairs from kGlyphPoints[first * 2]
struct GlyphEntry { const char* name; const char* shape; int firstStroke, strokeCount; };
struct NodeLook { const char* type; const char* shape; const char* glyph; float sizeW, sizeH; };

#include "app/node_looks_table.inc"

constexpr float kCharW = 6;      // the 10px monospace label font
constexpr float kPad = 8;        // from a socket to whatever is next to it
constexpr float kGlyphW = 28, kGlyphH = 16;
constexpr float kCircle = 36;    // the voices node: labels would not fit and are not shown
constexpr float kSpeakerW = 60;  // the output node's cone, with room for the L and R

TileShape shapeNamed(const std::string& name) {
  if (name == "rounded") return TileShape::Rounded;
  if (name == "circle") return TileShape::Circle;
  if (name == "speaker") return TileShape::Speaker;
  if (name == "dot") return TileShape::Dot;
  return TileShape::Box;
}

std::string shortPortName(const std::string& name) {
  if (name == "left") return "L";
  if (name == "right") return "R";
  return !name.empty() && name[0] == '@' ? name.substr(1) : name;
}

// The label of each port on one side: nothing when it is the only one.
std::vector<std::string> portLabels(const std::vector<std::string>& names) {
  if (names.size() == 1) return {""};
  std::vector<std::string> labels;
  for (const std::string& n : names) labels.push_back(shortPortName(n));
  return labels;
}

float widest(const std::vector<std::string>& labels) {
  std::size_t longest = 0;
  for (const std::string& s : labels) longest = std::max(longest, s.size());
  return static_cast<float>(longest) * kCharW;
}

const char* glyphShapeKey(TileShape shape) {
  switch (shape) {
    case TileShape::Speaker: return "speaker";
    case TileShape::Circle: return "circle";
    case TileShape::Rounded: return "rounded";
    default: return "box";
  }
}

}  // namespace

NodeLookInfo lookOf(const std::string& type) {
  static const std::map<std::string, NodeLookInfo> table = [] {
    std::map<std::string, NodeLookInfo> t;
    for (const NodeLook& l : kLooks) t[l.type] = {shapeNamed(l.shape), l.glyph, l.sizeW, l.sizeH};
    return t;
  }();
  const auto it = table.find(type);
  return it == table.end() ? NodeLookInfo{} : it->second;
}

TileLayout compactLayout(TileShape shape, const std::vector<std::string>& inputs, const std::vector<std::string>& outputs) {
  TileLayout t;
  t.inLabels = portLabels(inputs);
  t.outLabels = portLabels(outputs);
  const float rows = static_cast<float>(std::max<std::size_t>({inputs.size(), outputs.size(), 1}));
  if (shape == TileShape::Circle) {
    t.w = t.h = kCircle;
    t.glyphX = kCircle / 2; t.glyphW = t.glyphH = kCircle * 0.5f;
    for (std::string& s : t.inLabels) s.clear();
    for (std::string& s : t.outLabels) s.clear();
    return t;
  }
  t.h = std::max(rows * kCompactRow + 12, 32.0f);
  const float wIn = widest(t.inLabels), wOut = widest(t.outLabels);
  const float left = wIn > 0 ? kPad + wIn + 4 : kPad, right = wOut > 0 ? kPad + wOut + 4 : kPad;
  const float glyphW = shape == TileShape::Speaker ? 20 : kGlyphW;
  t.w = shape == TileShape::Speaker ? std::max(kSpeakerW, left + glyphW + right) : left + glyphW + right;
  t.glyphX = left + (t.w - left - right) / 2;
  t.glyphW = glyphW;
  t.glyphH = std::min(kGlyphH, t.h - 10);
  return t;
}

void drawGlyph(ImDrawList* dl, const std::string& name, TileShape shape, ImVec2 centre, float scale, ImU32 ink) {
  if (name.empty()) return;
  const char* key = glyphShapeKey(shape);
  const GlyphEntry* entry = nullptr;
  for (const GlyphEntry& g : kGlyphs) if (name == g.name && std::strcmp(g.shape, key) == 0) { entry = &g; break; }
  if (!entry) return;
  std::vector<ImVec2> pts;
  for (int s = 0; s < entry->strokeCount; s++) {
    const GlyphStroke& stroke = kGlyphStrokes[entry->firstStroke + s];
    pts.clear();
    for (int i = 0; i < stroke.count; i++) pts.push_back(ImVec2(centre.x + kGlyphPoints[(stroke.first + i) * 2] * scale, centre.y + kGlyphPoints[(stroke.first + i) * 2 + 1] * scale));
    const int a = static_cast<int>(255 * 0.85f * stroke.alpha);
    const ImU32 colour = (ink & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0, 255)) << 24);
    if (stroke.fill) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), colour);
    else dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colour, ImDrawFlags_None, std::max(1.0f, 1.25f * scale));
  }
}

}  // namespace wax::app
