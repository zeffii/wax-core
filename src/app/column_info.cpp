#include "app/column_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "app/units.h"
#include "core/node_registry.h"
#include "seq/names.h"

namespace wax::app {

namespace {

constexpr const char* kSeparator = " \xc2\xb7 ";

bool isBlank(const std::string& token) {
  return token.empty() || token.find_first_not_of('.') == std::string::npos;
}

std::string hexOf(double n, int width) {
  char text[16];
  std::snprintf(text, sizeof text, "%0*X", width, static_cast<unsigned>(std::max(0.0, std::round(n))));
  return text;
}

// The real value a hex cell stands for.
double scaled(bool isInt, bool log, double min, double max, double denom, double raw) {
  if (isInt) return std::min(max, min + raw);
  const double x = raw / denom;
  return log ? min * std::pow(max / min, x) : min + x * (max - min);
}

double unscaled(bool isInt, bool log, double min, double max, double denom, double value) {
  const double x = isInt ? value - min : log ? std::log(value / min) / std::log(max / min) * denom : (value - min) / (max - min) * denom;
  return std::clamp(std::round(x), 0.0, denom);
}

const Instrument* instrumentOf(const Project& project, const std::string& id) {
  for (const Instrument& i : project.instruments) if (i.id == id) return &i;
  return nullptr;
}

// The parameter's spec and the type of the node that holds it (the spec outlives the call: node types are static).
const ParamSpec* specOf(const Instrument& instrument, const std::string& nodeId, const std::string& param, std::string& type, const GraphNode*& node) {
  for (const GraphNode& n : instrument.graph.nodes) {
    if (n.id != nodeId) continue;
    const NodeType* nodeType = builtinNodes().find(n.type);
    if (!nodeType) return nullptr;
    type = n.type;
    node = &n;
    for (const ParamSpec& spec : nodeType->params) if (spec.name == param) return &spec;
  }
  return nullptr;
}

std::string rangeText(const std::string& type, const ParamSpec& spec, double min, double max, bool log) {
  return displayValue(type, spec, min) + " to " + displayValue(type, spec, max) + (log ? " (log)" : "");
}

// The last non-blank cell above the caret in a column ("" when there is none).
std::string heldAbove(const std::vector<std::vector<std::string>>& rows, int tick, int column) {
  for (int t = tick - 1; t >= 0; t--) {
    const auto& row = rows[static_cast<std::size_t>(t)];
    if (column < static_cast<int>(row.size()) && !isBlank(row[static_cast<std::size_t>(column)])) return row[static_cast<std::size_t>(column)];
  }
  return std::string();
}

}  // namespace

std::string describeColumn(const Project& project, const Lane& lane, const std::vector<Column>& columns,
                           const std::vector<std::vector<std::string>>& rows, int tick, int columnIndex) {
  if (columnIndex < 0 || columnIndex >= static_cast<int>(columns.size()) || tick < 0 || tick >= static_cast<int>(rows.size())) return std::string();
  const Column& c = columns[static_cast<std::size_t>(columnIndex)];
  const auto& row = rows[static_cast<std::size_t>(tick)];
  const std::string token = columnIndex < static_cast<int>(row.size()) ? row[static_cast<std::size_t>(columnIndex)] : std::string();
  const Instrument* instrument = instrumentOf(project, c.target.empty() ? lane.instrument : c.target);
  std::vector<std::string> parts;
  parts.push_back(c.longName.empty() ? c.label : c.longName);
  const int width = c.token == Token::Hex4 ? 4 : c.token == Token::Hex1 ? 1 : 2;

  if (c.kind == ColumnKind::Param && instrument) {
    std::string type;
    const GraphNode* node = nullptr;
    const ParamSpec* spec = specOf(*instrument, c.node, c.param, type, node);
    if (!spec) return parts.front();
    parts.push_back(rangeText(type, *spec, c.min, c.max, c.log));
    std::string hex = token, note;
    if (isBlank(token)) {
      hex = heldAbove(rows, tick, columnIndex);
      note = "held ";
      if (hex.empty()) {
        hex = hexOf(unscaled(c.isInt, c.log, c.min, c.max, c.denom, node->param(c.param, spec->def)), width);
        note = "default ";
      }
    }
    const double real = scaled(c.isInt, c.log, c.min, c.max, c.denom, static_cast<double>(std::stoul(hex, nullptr, 16)));
    parts.push_back(note + hex + " = " + displayValue(type, *spec, real));
  } else if (c.kind == ColumnKind::Vel || c.kind == ColumnKind::Trig) {
    const bool vel = c.kind == ColumnKind::Vel;
    parts.push_back(vel ? "00 to FF" : "1 to F (0 is nothing)");
    std::string hex = token, note;
    if (isBlank(token) && vel) { hex = heldAbove(rows, tick, columnIndex); note = hex.empty() ? "default " : "held "; if (hex.empty()) hex = "FF"; }
    if (!isBlank(hex)) {
      const double percent = std::stoul(hex, nullptr, 16) / (vel ? 255.0 : 15.0) * 100;
      char text[48];
      std::snprintf(text, sizeof text, "%.0f%%", percent);
      parts.push_back(note + hex + " = " + text);
    }
  } else if (c.kind == ColumnKind::Cmd) {
    if (isBlank(token) || token.size() < 6) {
      parts.push_back("CC = command, XXXX = value");
    } else {
      const int id = static_cast<int>(std::stoul(token.substr(0, 2), nullptr, 16));
      const Command* command = nullptr;
      if (instrument) for (const Command& k : instrument->commands) if (k.id == id) command = &k;
      const CommandTarget* target = nullptr;
      for (const CommandTarget& k : c.commands) if (k.id == id) target = &k;
      if (command && target && target->reset) {
        parts.push_back(token.substr(0, 2) + ": reset settings");
      } else if (command && target && !target->setting && instrument) {
        std::string type;
        const GraphNode* node = nullptr;
        const ParamSpec* spec = specOf(*instrument, command->node, command->param, type, node);
        parts.push_back(token.substr(0, 2) + ": " + longName(command->param));
        if (spec) {
          parts.push_back(rangeText(type, *spec, target->min, target->max, target->log));
          const double raw = static_cast<double>(std::stoul(token.substr(2), nullptr, 16));
          parts.push_back(token.substr(2) + " = " + displayValue(type, *spec, scaled(target->isInt, target->log, target->min, target->max, 65535, raw)));
        }
      } else if (id == 0 && !c.presets.empty()) {
        parts.push_back("00: recall preset");
        parts.push_back("number " + std::to_string(std::stoul(token.substr(2), nullptr, 16)));
      } else if (command && target && target->setting) {   // saves or loads the settings of its group
        parts.push_back(token.substr(0, 2) + (target->setting == 1 ? ": save settings" : ": load settings"));
        parts.push_back("number " + std::to_string(std::stoul(token.substr(2), nullptr, 16)));
      } else {   // a number the instrument has no command for
        parts.push_back(token.substr(0, 2) + ": no such command in this instrument");
      }
    }
  }
  std::string out;
  for (std::size_t i = 0; i < parts.size(); i++) out += (i ? kSeparator : "") + parts[i];
  return out;
}

}  // namespace wax::app
