#include "app/pattern_colour.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace wax::app {

namespace {

constexpr int kPresets = 16;
constexpr int kPastelHue[kPresets] = {0, 22, 45, 68, 90, 112, 135, 158, 180, 202, 225, 248, 270, 292, 315, 338};
struct Hsl { int h, s, l; };
constexpr Hsl kAuto[6] = {{210, 70, 55}, {150, 60, 45}, {30, 80, 55}, {280, 60, 60}, {0, 70, 55}, {55, 70, 50}};

ImU32 fromHsl(float h, float s, float l, int alpha) {
  s /= 100; l /= 100;
  const float c = (1 - std::fabs(2 * l - 1)) * s, hp = std::fmod(h, 360.0f) / 60.0f;
  const float x = c * (1 - std::fabs(std::fmod(hp, 2.0f) - 1)), m = l - c / 2;
  float r = 0, g = 0, b = 0;
  if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
  else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
  return IM_COL32(static_cast<int>((r + m) * 255 + 0.5f), static_cast<int>((g + m) * 255 + 0.5f), static_cast<int>((b + m) * 255 + 0.5f), alpha);
}

}  // namespace

ImU32 patternColour(const Pattern& pattern, int alpha) {
  const Json& c = pattern.color;
  if (c.is_number_integer() && c.get<int>() >= 0 && c.get<int>() < kPresets) return fromHsl(static_cast<float>(kPastelHue[c.get<int>()]), 65, 72, alpha);
  if (c.is_string() && c.get<std::string>().size() == 7 && c.get<std::string>()[0] == '#') {
    const unsigned long v = std::strtoul(c.get<std::string>().c_str() + 1, nullptr, 16);
    return IM_COL32((v >> 16) & 255, (v >> 8) & 255, v & 255, alpha);
  }
  const Hsl a = kAuto[static_cast<std::size_t>(pattern.index % 6)];
  return fromHsl(static_cast<float>(a.h), static_cast<float>(a.s), static_cast<float>(a.l), alpha);
}

Json steppedPatternColor(const Pattern& pattern, int delta) {
  const int at = pattern.color.is_number_integer() ? pattern.color.get<int>() : -1;   // -1: automatic
  const int count = kPresets + 1;
  const int next = ((at + 1 + delta) % count + count) % count - 1;
  return next < 0 ? Json() : Json(next);
}

}  // namespace wax::app
