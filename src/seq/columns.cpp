#include "seq/columns.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wax {
namespace {

// bits: 16 is a four-digit column, 4 a one-digit column (a toggle or an enum), otherwise two digits.
Token tokenForBits(int bits) { return bits == 16 ? Token::Hex4 : bits == 4 ? Token::Hex1 : Token::Hex2; }

double denomOf(Token token) { return token == Token::Hex4 ? 65535 : token == Token::Hex1 ? 15 : 255; }

int notePairs(const Instrument& instrument) {
  return instrument.noteColumns == 0 ? 0 : std::max(1, std::min(8, instrument.noteColumns));   // none for the master lane
}

Column paramColumn(int slot, const ParamRegistry& registry, int bits, bool log, const std::string& node, const std::string& param) {
  const ParamMeta& meta = registry.meta(slot);
  Column column;
  column.kind = ColumnKind::Param;
  column.token = tokenForBits(bits);
  column.slot = slot;
  column.min = meta.min;
  column.max = meta.max;
  column.isInt = meta.isInt;
  column.denom = denomOf(column.token);
  column.log = log;
  column.node = node;
  column.param = param;
  return column;
}

const GraphNode* findNode(const Instrument& instrument, const std::string& id) {
  for (const GraphNode& n : instrument.graph.nodes) if (n.id == id) return &n;
  return nullptr;
}

// The command column: the table of commands, the presets command 00 recalls, and the groups a reset command puts back.
Column commandColumn(const Instrument& instrument, const ParamRegistry& registry) {
  Column column;
  column.kind = ColumnKind::Cmd;
  column.token = Token::Cmd;

  for (const Command& c : instrument.commands) {
    CommandTarget target;
    target.id = c.id;
    if (c.reset || !c.setting.empty()) {
      target.reset = c.reset;
      target.setting = c.setting == "save" ? 1 : c.setting.empty() ? 0 : 2;
    } else {
      const int slot = registry.slotOf(instrument.id + "." + c.node + "." + c.param);
      if (slot < 0) throw std::runtime_error("\"" + instrument.id + "\" does not expose " + c.node + "." + c.param);
      const ParamMeta& meta = registry.meta(slot);
      target.slot = slot;
      target.min = meta.min;
      target.max = meta.max;
      target.isInt = meta.isInt;
      target.log = c.log;
      target.fixed = c.fixed;
    }
    column.commands.push_back(target);
  }

  // Parameters that no longer exist are left out of a preset. (The drawn curves a preset holds wait for node messages.)
  for (const Json& p : instrument.raw.value("presets", Json::array())) {
    PresetValues preset;
    preset.id = p.value("id", 0);
    const Json values = p.value("values", Json::object());
    for (const auto& [key, value] : values.items()) {
      const int slot = registry.slotOf(instrument.id + "." + key);
      if (slot >= 0 && value.is_number() && std::isfinite(value.get<double>())) {
        preset.slots.push_back(slot);
        preset.values.push_back(value.get<double>());
      }
    }
    column.presets.push_back(std::move(preset));
  }

  const Json groups = instrument.raw.value("commandGroups", Json::object());
  const GraphNode* sampler = findNode(instrument, "sampler");
  for (const auto& [bit, names] : groups.items()) {
    std::vector<GroupValue>& list = column.groups[std::stoi(bit)];
    for (const Json& name : names) {
      const std::string param = name.get<std::string>();
      const int slot = registry.slotOf(instrument.id + ".sampler." + param);
      if (slot < 0) continue;
      list.push_back({slot, sampler ? sampler->param(param, registry.values()[slot]) : registry.values()[slot]});
    }
  }
  return column;
}

// A meta machine plays other instruments: each of its columns is a proxy for one thing of another instrument
// (its notes, a velocity, a trigger, or a shown parameter), so one lane can drive many machines.
std::vector<Column> metaColumns(const Instrument& self, const Project& project, const ParamRegistry& registry) {
  const auto usable = [&](const MetaColumn& spec) { return spec.instrument != self.id && project.findInstrument(spec.instrument) != nullptr; };

  // where each instrument's note columns stand among the lane's, so a velocity finds its note wherever it was put
  std::map<std::pair<std::string, int>, int> noteAt;
  for (const MetaColumn& spec : self.metaColumns) {
    if ((spec.role == "note" || spec.role == "rnote") && usable(spec)) {
      const std::pair<std::string, int> key = {spec.instrument, spec.nc};
      if (!noteAt.count(key)) { const int next = static_cast<int>(noteAt.size()); noteAt[key] = next; }
    }
  }

  std::vector<Column> columns;
  int notes = 0, trigs = 0;
  for (const MetaColumn& spec : self.metaColumns) {
    if (spec.role == "spacer") {   // a gap between groups of columns
      Column gap;
      gap.kind = ColumnKind::Spacer;
      gap.token = Token::Spacer;
      columns.push_back(gap);
      continue;
    }
    if (!usable(spec)) continue;   // a machine that is gone has no column
    const Instrument& target = *project.findInstrument(spec.instrument);
    Column column;
    column.target = spec.instrument;
    if (spec.role == "note" || spec.role == "rnote") {
      column.kind = ColumnKind::Note;
      column.rel = spec.role == "rnote";
      column.token = column.rel ? Token::Hex2 : Token::Note;
      column.root = spec.root;
      column.nc = notes++;
    } else if (spec.role == "vel") {
      const auto partner = noteAt.find({spec.instrument, spec.nc});
      if (partner == noteAt.end()) continue;
      column.kind = ColumnKind::Vel;
      column.token = Token::Hex2;
      column.nc = partner->second;
    } else if (spec.role == "trig") {
      if (spec.tc < 0 || spec.tc >= static_cast<int>(target.triggers.size())) continue;
      column.kind = ColumnKind::Trig;
      column.token = Token::Hex1;
      column.tc = trigs++;
      column.note = target.triggers[static_cast<std::size_t>(spec.tc)].note;
    } else if (spec.role == "param") {
      const auto shown = std::find_if(target.exposed.begin(), target.exposed.end(), [&](const Exposed& e) { return e.node == spec.node && e.param == spec.param; });
      const int slot = registry.slotOf(spec.instrument + "." + spec.node + "." + spec.param);
      if (shown == target.exposed.end() || slot < 0) continue;
      column = paramColumn(slot, registry, spec.bits ? spec.bits : shown->bits, shown->log, spec.node, spec.param);
      column.target = spec.instrument;
    } else {
      continue;
    }
    columns.push_back(std::move(column));
  }
  return columns;
}

// The instrument's own columns: note and velocity (repeated for each note column), its triggers, one column per shown
// parameter, then the command column if the instrument has a command table.
std::vector<Column> ownColumns(const Instrument& instrument, const ParamRegistry& registry) {
  std::vector<Column> columns;
  for (int k = 0; k < notePairs(instrument); k++) {
    Column note;
    note.kind = ColumnKind::Note;
    note.token = Token::Note;
    note.nc = k;
    columns.push_back(note);
    if (!instrument.noVel) {   // a MOD channel has no velocity
      Column vel;
      vel.kind = ColumnKind::Vel;
      vel.token = Token::Hex2;
      vel.nc = k;
      columns.push_back(vel);
    }
  }
  // Trigger columns (the hi-hat's Cl and Op): one hex digit each, 1 to F the loudness of a hit.
  for (std::size_t i = 0; i < instrument.triggers.size(); i++) {
    Column trig;
    trig.kind = ColumnKind::Trig;
    trig.token = Token::Hex1;
    trig.tc = static_cast<int>(i);
    trig.note = instrument.triggers[i].note;
    columns.push_back(trig);
  }
  for (const Exposed& e : instrument.exposed) {
    const int slot = registry.slotOf(instrument.id + "." + e.node + "." + e.param);
    if (slot < 0) throw std::runtime_error("\"" + instrument.id + "\" does not expose " + e.node + "." + e.param);
    columns.push_back(paramColumn(slot, registry, e.bits, e.log, e.node, e.param));
  }
  const bool hasPresets = !instrument.raw.value("presets", Json::array()).empty();
  if (!instrument.commands.empty() || hasPresets) columns.push_back(commandColumn(instrument, registry));
  return columns;
}

}  // namespace

std::vector<Column> buildColumns(const Instrument& instrument, const Project& project, const ParamRegistry& registry) {
  std::vector<Column> columns = instrument.isMeta ? metaColumns(instrument, project, registry) : ownColumns(instrument, registry);
  int ch = 0;
  for (Column& c : columns) {   // where each column starts in the lane's channels
    c.ch = ch;
    ch += tokenChannels(c.token);
  }
  return columns;
}

int channelCount(const std::vector<Column>& columns) {
  int channels = 0;
  for (const Column& c : columns) channels += tokenChannels(c.token);
  return channels;
}

}  // namespace wax
