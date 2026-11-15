#include "app/units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wax::app {

namespace {

struct UnitEntry { const char* type; const char* name; const char* unit; };
struct ChoiceEntry { const char* type; const char* name; std::vector<std::string> names; };

#include "app/units_table.inc"

std::string trim(double v, int digits) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*g", digits, v);
  return buf;
}

std::string signed_(double v, const std::string& text) { return v > 0 ? "+" + text : text; }

}  // namespace

std::string unitOf(const std::string& type, const std::string& name) {
  for (const UnitEntry& e : kTypeUnits) if (type == e.type && name == e.name) return e.unit;
  for (const UnitEntry& e : kNameTableUnits) if (type == e.type && name == e.name) return e.unit;
  for (const UnitEntry& e : kNameUnits) if (name == e.name) return e.unit;
  return {};
}

const std::vector<std::string>& choicesOf(const std::string& type, const std::string& name) {
  static const std::vector<std::string> none;
  for (const ChoiceEntry& e : kChoices) if (type == e.type && name == e.name) return e.names;
  return none;
}

std::string displayValue(const std::string& type, const ParamSpec& spec, double value) {
  const std::vector<std::string>& names = choicesOf(type, spec.name);
  const long index = std::lround(value) - std::lround(spec.min);
  if (!names.empty()) return index >= 0 && index < static_cast<long>(names.size()) ? names[static_cast<std::size_t>(index)] : std::to_string(std::lround(value));
  if (spec.isInt && spec.max - spec.min <= 1 && spec.min == 0) return std::lround(value) == 0 ? "off" : "on";
  const std::string u = unitOf(type, spec.name);
  if (u == "%") return std::to_string(std::lround(value * 100)) + "%";
  if (u == "ms") return std::fabs(value) >= 1000 ? trim(value / 1000, 3) + " s" : trim(value, 3) + " ms";
  if (u == "Hz") return std::fabs(value) >= 1000 ? trim(value / 1000, 3) + " kHz" : trim(value, 3) + " Hz";
  if (u == "st" || u == "ct" || u == "dB" || u == "oct" || u == "dB/oct") return signed_(value, trim(value, 3)) + " " + u;
  if (u == ":1") return trim(value, 3) + ":1";
  if (u == "x") return trim(value, 3) + "x";
  if (u.empty()) return trim(value, 4);
  return trim(value, 4) + " " + u;
}

bool parseValue(const std::string& type, const ParamSpec& spec, const std::string& text, double& value) {
  const std::vector<std::string>& names = choicesOf(type, spec.name);
  for (std::size_t i = 0; i < names.size(); i++) if (names[i] == text) { value = spec.min + static_cast<double>(i); return true; }
  if (text == "on") { value = 1; return true; }
  if (text == "off") { value = 0; return true; }
  char* end = nullptr;
  double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return false;
  std::string rest(end);
  rest.erase(std::remove(rest.begin(), rest.end(), ' '), rest.end());
  const std::string u = unitOf(type, spec.name);
  if (rest == "k" || rest == "kHz") v *= 1000;
  else if (rest == "s" && u == "ms") v *= 1000;
  else if (u == "%" && (rest == "%" || rest.empty())) v /= 100;
  else if (!rest.empty() && rest != u && rest != "ms" && rest != "Hz" && rest != "st" && rest != "ct" && rest != "dB") return false;
  value = std::clamp(spec.isInt ? std::round(v) : v, spec.min, spec.max);
  return true;
}

}  // namespace wax::app
