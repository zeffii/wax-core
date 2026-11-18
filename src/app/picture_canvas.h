// Drawing the node pictures in "web pixels": a Canvas is a rectangle of the screen whose top left is (0, 0) and whose unit is one pixel
// of the web app's picture, times the scale; the pictures that are ports of the web's canvas panels (pogue, the space views) are written
// with it so their numbers read as the web's. Also the colours those panels share and the helpers that make parts of a picture
// something the mouse can take hold of (a handle, a button, a region).
#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "app/node_response.h"
#include "app/theme.h"
#include "core/node.h"

namespace wax::app {

inline ImU32 withAlphaF(ImU32 colour, float alpha) { return (colour & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255) << 24); }

// hue 0..360, saturation and lightness 0..1
inline ImU32 hslColour(float hue, float saturation, float lightness) {
  const float h = std::fmod(hue, 360.0f) / 60, k = (1 - std::fabs(2 * lightness - 1)) * saturation, x = k * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = lightness - k / 2;
  float r = 0, g = 0, b = 0;
  if (h < 1) { r = k; g = x; } else if (h < 2) { r = x; g = k; } else if (h < 3) { g = k; b = x; } else if (h < 4) { g = x; b = k; } else if (h < 5) { r = x; b = k; } else { r = k; b = x; }
  return IM_COL32(static_cast<int>((r + m) * 255), static_cast<int>((g + m) * 255), static_cast<int>((b + m) * 255), 255);
}

// the colours of the web's panels (spaceui.js)
inline ImU32 inkOrange() { return hslColour(30, 0.9f, 0.58f); }
inline ImU32 inkBlue() { return hslColour(195, 0.75f, 0.58f); }
inline ImU32 inkPink() { return hslColour(335, 0.8f, 0.62f); }

struct Canvas {
  ImDrawList* dl;
  ImFont* font;
  ImVec2 origin;   // the canvas's top left, on the screen
  float scale;     // screen pixels for one web pixel

  ImVec2 at(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
  float lineWidth(float width) const { return std::max(1.0f, width * scale); }
  void line(float x0, float y0, float x1, float y1, ImU32 colour, float width = 1) const { dl->AddLine(at(x0, y0), at(x1, y1), colour, lineWidth(width)); }
  // a dashed line: `on` drawn, `off` skipped, in web pixels
  void dashed(float x0, float y0, float x1, float y1, ImU32 colour, float width, float on, float off) const {
    const float length = std::hypot(x1 - x0, y1 - y0);
    for (float d = 0; d < length; d += on + off) {
      const float a = d / length, b = std::min(length, d + on) / length;
      line(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a, x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, colour, width);
    }
  }
  void rect(float x, float y, float w, float h, ImU32 colour) const { dl->AddRect(at(x, y), at(x + w, y + h), colour, 0, 0, lineWidth(1)); }
  void fill(float x, float y, float w, float h, ImU32 colour) const { dl->AddRectFilled(at(x, y), at(x + w, y + h), colour); }
  void dot(float x, float y, float radius, ImU32 colour) const { dl->AddCircleFilled(at(x, y), radius * scale, colour); }
  void circle(float x, float y, float radius, ImU32 colour, float width = 1) const { dl->AddCircle(at(x, y), radius * scale, colour, 0, lineWidth(width)); }
  void polyline(const std::vector<ImVec2>& points, ImU32 colour, float width) const {   // points already in screen pixels
    if (points.size() > 1) dl->AddPolyline(points.data(), static_cast<int>(points.size()), colour, 0, lineWidth(width));
  }
  // an arc of a circle, angles in radians (0 to the right, clockwise on the screen)
  void arc(float x, float y, float radius, float from, float to, ImU32 colour, float width = 1) const {
    dl->PathArcTo(at(x, y), radius * scale, from, to);
    dl->PathStroke(colour, 0, lineWidth(width));
  }
  float textSize() const { return std::max(9 * scale, 7.0f); }
  // the width of a text in web pixels
  float textWidth(const std::string& text) const { return font->CalcTextSizeA(textSize(), FLT_MAX, 0, text.c_str()).x / scale; }
  // small text at (x, y); align 0 starts there, 1 ends there, 2 is centred on it
  void label(const std::string& text, float x, float y, ImU32 colour, float alpha = 0.6f, int align = 0) const {
    const float width = font->CalcTextSizeA(textSize(), FLT_MAX, 0, text.c_str()).x;
    const ImVec2 p = at(x, y);
    dl->AddText(font, textSize(), ImVec2(align == 1 ? p.x - width : align == 2 ? p.x - width / 2 : p.x, p.y), withAlphaF(colour, alpha), text.c_str());
  }
};

inline std::string hertz(double v) {
  char text[24];
  if (v >= 1000) std::snprintf(text, sizeof text, v >= 10000 ? "%.0f kHz" : "%.1f kHz", v / 1000);
  else std::snprintf(text, sizeof text, "%.0f Hz", v);
  return text;
}
inline std::string number(double v, int digits = 1) { char text[24]; std::snprintf(text, sizeof text, "%.*f", digits, v); return text; }

// ---- what the mouse can take hold of ----

// A handle: where it is grabbed (`at`) and the rectangle (screen) that the position it is dragged to is measured in. Some pictures use the
// rectangle's fields for other things (see their drag functions).
inline void addHandle(ResponseHandles& handles, const GraphNode& node, const std::string& param, ImVec2 at, ImVec2 rectMin = ImVec2(0, 0), ImVec2 rectSize = ImVec2(1, 1), bool once = false) {
  ResponseHandles::Handle handle{node.id, param, 0, at};
  handle.gx = rectMin.x; handle.gy = rectMin.y; handle.gw = rectSize.x; handle.gh = rectSize.y; handle.once = once;
  handles.list.push_back(handle);
}

// a line that is grabbed anywhere along it: handles every few pixels
inline void addLineHandles(ResponseHandles& handles, const GraphNode& node, const std::string& param, const Canvas& c, float y, float x0, float x1, ImVec2 rectMin, ImVec2 rectSize) {
  for (float x = x0; x <= x1; x += 12) addHandle(handles, node, param, c.at(x, y), rectMin, rectSize);
}

// a region that is grabbed anywhere in it: a grid of handles (web pixels from x0, y0 to x1, y1)
inline void addRegionHandles(ResponseHandles& handles, const GraphNode& node, const std::string& param, const Canvas& c, float x0, float y0, float x1, float y1, ImVec2 rectMin, ImVec2 rectSize) {
  for (float y = y0; y <= y1; y += 14)
    for (float x = x0; x <= x1; x += 14) addHandle(handles, node, param, c.at(x, y), rectMin, rectSize);
}

// A button in the picture: a thin outline (the accent colour when `on`) round its text; a click does `action`, one of
//   "@set:name=value"   set the parameter        "@flip:name"   switch a 0/1 parameter        "@nudge:name=by"   add to it (within its range)
struct PictureButton { std::string text, action; bool on = false; };

inline void drawButton(const Canvas& c, ResponseHandles& handles, const GraphNode& node, const PictureButton& button, float x, float y, float w, float h, ImU32 accent) {
  c.rect(x + 0.5f, y + 0.5f, w - 1, h - 1, withAlphaF(button.on ? accent : kText, button.on ? 0.9f : 0.35f));
  c.label(button.text, x + w / 2, y + (h - 9) / 2, button.on ? accent : kText, button.on ? 1.0f : 0.7f, 2);
  for (float px = x + 6; px < x + w; px += 12) addHandle(handles, node, button.action, c.at(px, y + h / 2), ImVec2(0, 0), ImVec2(1, 1), true);
  addHandle(handles, node, button.action, c.at(x + w / 2, y + h / 2), ImVec2(0, 0), ImVec2(1, 1), true);
}

}  // namespace wax::app
