#include "engine/sequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "core/js_math.h"
#include "seq/cells.h"

namespace wax {

// Command table for a CCXXYY column: id -> parameter target, as flat arrays so that playing a row does no lookups.
struct Sequencer::CommandTable {
  std::array<int, 256> slot;
  std::array<double, 256> min{}, max{};
  std::array<bool, 256> log{}, isInt{};
  std::array<double, 256> fix;            // a command that writes one value whatever its value cell says (NaN: none)
  std::array<int, 256> reset{};           // a group mask
  std::array<int, 256> setting{};         // 1 save, 2 load
  std::map<int, PresetValues> presets;    // recalled by command 00
  std::map<int, std::vector<GroupValue>> groups;

  explicit CommandTable(const Column& column) : presets(), groups(column.groups) {
    slot.fill(-1);
    fix.fill(std::numeric_limits<double>::quiet_NaN());
    for (const PresetValues& p : column.presets) presets[p.id] = p;
    for (const CommandTarget& c : column.commands) {
      if (c.id < 0 || c.id > 255) continue;
      const std::size_t id = static_cast<std::size_t>(c.id);
      if (c.reset) reset[id] = c.reset;
      if (c.setting) setting[id] = c.setting;
      slot[id] = c.slot;
      min[id] = c.min;
      max[id] = c.max;
      log[id] = c.log;
      isInt[id] = c.isInt;
      if (c.fixed && std::isfinite(*c.fixed)) fix[id] = *c.fixed;
    }
  }

  // The parameters of every group whose bit is set in the mask.
  std::vector<GroupValue> inGroups(int mask) const {
    std::vector<GroupValue> list;
    for (const auto& [bit, values] : groups) if (mask & bit) list.insert(list.end(), values.begin(), values.end());
    return list;
  }
};

// A lane as the sequencer plays it.
struct Sequencer::LaneState {
  std::string id;
  std::string instrument;
  std::vector<std::vector<float>> pages;
  std::vector<std::uint8_t> mask;
  bool muted = false;
  std::vector<Column> columns;
  std::vector<std::unique_ptr<CommandTable>> tables;   // one per column, nullptr where the column is not a command column
  int channels = 0;
  int noteCols = 0;

  std::vector<int> prev;                  // the note each note column last started, or -1
  std::vector<float> notes;               // this row's note cells
  std::vector<float> vels;                // this row's velocities (kept as floats, as the JS keeps them)
  std::vector<std::string> noteTargets;   // the machine each note column plays (a meta machine's columns play other machines)
  std::vector<std::string> targets;       // every machine the lane's columns name; empty when none does
  std::vector<int> columnKeys;            // which note column a note came from, for machines that keep a column on one voice
  std::vector<const Column*> trigCols;
  std::vector<float> trigs;               // this row's trigger cells

  // 70 save, 71 load: sixteen places to keep a set of parameter values
  std::array<std::vector<std::pair<int, float>>, 16> settings;
};

Sequencer::Sequencer(double sampleRate, GraphRuntime& runtime, int maxBlock)
    : sampleRate_(sampleRate),
      runtime_(runtime),
      params_(runtime.params()),
      limiter_(sampleRate),
      scratchLeft_(static_cast<std::size_t>(maxBlock), 0.0f),
      scratchRight_(static_cast<std::size_t>(maxBlock), 0.0f) {}

Sequencer::~Sequencer() = default;

void Sequencer::setSong(const SongSettings& song) {
  const SongSettings old = song_;
  const bool hadMaster = songSet_ && old.master.present();
  song_ = song;
  songSet_ = true;
  runtime_.masterSlots = song.master;
  const bool changed = !hadMaster || old.bpm != song.bpm || old.ticksPerBeat != song.ticksPerBeat || old.swing != song.swing;
  if (song.master.present() && changed) resetTempo();
  runtime_.clock().tickSamples = (sampleRate_ * 60) / (song.bpm * song.ticksPerBeat);
}

void Sequencer::resetTempo() {
  const MasterSlots& m = song_.master;
  if (!m.present()) return;
  params_[m.bpm] = static_cast<float>(song_.bpm);
  params_[m.swing] = static_cast<float>(jsRound(song_.swing * 100));
  params_[m.tpb] = static_cast<float>(song_.ticksPerBeat);
  params_[m.volume] = 1;
}

// The tempo now: the master lane's values when there is one, else the song's.
Sequencer::Tempo Sequencer::tempo() const {
  const MasterSlots& m = song_.master;
  if (!m.present()) return {song_.bpm, clamp(song_.swing, 0.5, 0.75), song_.ticksPerBeat};
  return {
    clamp(params_[m.bpm], 20, 999),
    clamp(params_[m.swing] / 100.0, 0.5, 0.75),
    static_cast<int>(clamp(jsRound(params_[m.tpb]), 1, 48)),
  };
}

void Sequencer::setLane(const PreparedLane& prepared) {
  auto fresh = std::make_unique<LaneState>();
  LaneState& lane = *fresh;
  lane.id = prepared.id;
  lane.instrument = prepared.instrument;
  lane.pages = prepared.pages;
  lane.mask = prepared.mask;
  lane.columns = prepared.columns;
  for (const Column& c : lane.columns) {
    lane.tables.push_back(c.kind == ColumnKind::Cmd ? std::make_unique<CommandTable>(c) : nullptr);
    lane.channels = std::max(lane.channels, c.ch + (c.kind == ColumnKind::Cmd ? 2 : 1));
    if (c.kind == ColumnKind::Note) {
      lane.noteCols++;
      lane.noteTargets.push_back(c.target.empty() ? lane.instrument : c.target);
      lane.columnKeys.push_back(nextColumnKey_++);
    }
    if (c.kind == ColumnKind::Trig) lane.trigCols.push_back(&c);
    if (!c.target.empty() && std::find(lane.targets.begin(), lane.targets.end(), c.target) == lane.targets.end()) lane.targets.push_back(c.target);
  }
  lane.prev.assign(static_cast<std::size_t>(lane.noteCols), -1);
  lane.notes.assign(static_cast<std::size_t>(lane.noteCols), kEmpty);
  lane.vels.assign(static_cast<std::size_t>(lane.noteCols), 1.0f);
  lane.trigs.assign(lane.trigCols.size(), kEmpty);

  // A lane that is replaced while it plays keeps what it was sounding, so the note-offs still find their notes;
  // with a different set of note columns everything of that machine is let go instead.
  for (auto& existing : lanes_) {
    if (existing->id != lane.id) continue;
    if (existing->instrument == lane.instrument) {
      if (existing->prev.size() == lane.prev.size()) {
        lane.prev = existing->prev;
        lane.muted = existing->muted;
        lane.settings = existing->settings;
        lane.columnKeys = existing->columnKeys;
      } else if (std::any_of(existing->prev.begin(), existing->prev.end(), [](int n) { return n >= 0; })) {
        allOff(*existing);
      }
    }
    existing = std::move(fresh);
    return;
  }
  lanes_.push_back(std::move(fresh));
}

void Sequencer::play(int from, bool once) {
  if (playing_) releaseAll();   // jumping while playing: what rings is let go first
  resetTempo();
  tick_ = from >= 0 && from < song_.length ? from : 0;
  songPos_ = tick_;
  countdown_ = 0;
  once_ = once;
  wrapped_ = false;
  playing_ = true;
}

// Stopping lets every voice fade out over its release time.
void Sequencer::stop() {
  releaseAll();
  playing_ = false;
}

// Lets go of everything a lane is sounding, on every machine it plays.
void Sequencer::allOff(LaneState& lane) {
  NoteEvent all;
  all.type = NoteType::AllOff;
  if (lane.targets.empty()) runtime_.note(lane.instrument, all);
  else for (const std::string& target : lane.targets) runtime_.note(target, all);
}

void Sequencer::releaseAll() {
  for (auto& lane : lanes_) {
    allOff(*lane);
    std::fill(lane->prev.begin(), lane->prev.end(), -1);
  }
}

// implied: the release comes from a new note in the same column, not from a note off in the pattern.
void Sequencer::release(LaneState& lane, int k, bool implied) {
  const std::size_t column = static_cast<std::size_t>(k);
  if (lane.prev[column] < 0) return;
  NoteEvent off;
  off.type = NoteType::Off;
  off.note = lane.prev[column];
  off.implied = implied;
  runtime_.note(lane.noteTargets[column], off);
  lane.prev[column] = -1;
}

// x is 0..1; the slot gets the value on the parameter's own scale.
void Sequencer::writeParam(int slot, double min, double max, bool log, double x) {
  const double value = log ? min * std::pow(max / min, x) : min + x * (max - min);
  if (std::isfinite(value)) params_[slot] = static_cast<float>(value);
  touch(slot);
}

// Counts a pattern's write to a slot (nodes watch the count to tell a new event from an unchanged value).
void Sequencer::touch(int slot) { runtime_.clock().stamps[static_cast<std::size_t>(slot)]++; }

void Sequencer::playRow(LaneState& lane) {
  const std::size_t pageIndex = static_cast<std::size_t>(tick_ / kPageTicks);
  if (pageIndex >= lane.pages.size()) return;
  const std::vector<float>& page = lane.pages[pageIndex];
  const std::size_t base = static_cast<std::size_t>(tick_ % kPageTicks) * static_cast<std::size_t>(lane.channels);
  std::fill(lane.notes.begin(), lane.notes.end(), kEmpty);
  std::fill(lane.vels.begin(), lane.vels.end(), 1.0f);
  std::fill(lane.trigs.begin(), lane.trigs.end(), kEmpty);

  for (std::size_t c = 0; c < lane.columns.size(); c++) {
    const Column& col = lane.columns[c];
    const float v = page[base + static_cast<std::size_t>(col.ch)];
    if (v == kEmpty) continue;
    switch (col.kind) {
      case ColumnKind::Param:
        // A whole-number parameter (toggle, enum) takes the number in the cell itself, at any width.
        if (col.isInt) { params_[col.slot] = static_cast<float>(std::min(col.max, col.min + jsRound(v * col.denom))); touch(col.slot); }
        else writeParam(col.slot, col.min, col.max, col.log, jsRound(v * col.denom) / col.denom);
        break;
      case ColumnKind::Cmd: {
        // Needs both halves: the command id and its value.
        const float value = page[base + static_cast<std::size_t>(col.ch) + 1];
        const CommandTable& t = *lane.tables[c];
        const std::size_t id = static_cast<std::size_t>(std::max(0, std::min(255, decodeHex2(v))));
        if (t.reset[id]) {   // Ax: back to the starting values
          for (const GroupValue& g : t.inGroups(t.reset[id])) { params_[g.slot] = static_cast<float>(g.value); touch(g.slot); }
        } else if (t.setting[id] && value != kEmpty) {   // 70 save, 71 load: xx the groups, yy the place of 16
          const int x = decodeHex4(value);
          const std::vector<GroupValue> list = t.inGroups((x >> 8) & 255);
          auto& place = lane.settings[static_cast<std::size_t>(x & 15)];
          if (t.setting[id] == 1) {
            place.clear();
            for (const GroupValue& g : list) place.push_back({g.slot, params_[g.slot]});
          } else {
            for (const auto& [slot, saved] : place) {
              const bool inList = std::any_of(list.begin(), list.end(), [slot = slot](const GroupValue& g) { return g.slot == slot; });
              if (inList) { params_[slot] = saved; touch(slot); }
            }
          }
        } else if (t.slot[id] >= 0 && !std::isnan(t.fix[id])) {   // e.g. F0: back to relative offsets
          params_[t.slot[id]] = static_cast<float>(t.fix[id]);
          touch(t.slot[id]);
        } else if (value != kEmpty && t.slot[id] >= 0) {
          // 0003 is wave 3 for a whole-number target; otherwise the value runs across the range.
          if (t.isInt[id]) { params_[t.slot[id]] = static_cast<float>(std::min(t.max[id], t.min[id] + decodeHex4(value))); touch(t.slot[id]); }
          else writeParam(t.slot[id], t.min[id], t.max[id], t.log[id], decodeHex4(value) / 65535.0);
        } else if (value != kEmpty && id == 0 && !t.presets.empty()) {
          // Command 00 recalls the preset numbered by the value; an unknown number does nothing.
          const auto preset = t.presets.find(decodeHex4(value));
          if (preset != t.presets.end()) {
            for (std::size_t i = 0; i < preset->second.slots.size(); i++) {
              params_[preset->second.slots[i]] = static_cast<float>(preset->second.values[i]);
              touch(preset->second.slots[i]);
            }
            // (the drawn curves and spectra a preset holds are sent to its nodes here, once node messages are ported)
          }
        }
        break;
      }
      case ColumnKind::Trig: lane.trigs[static_cast<std::size_t>(col.tc)] = v; break;
      case ColumnKind::Vel: lane.vels[static_cast<std::size_t>(col.nc)] = static_cast<float>(decodeHex2(v) / 255.0); break;
      case ColumnKind::Note: lane.notes[static_cast<std::size_t>(col.nc)] = col.rel ? relativeNote(v, col.root) : v; break;
      case ColumnKind::Spacer: break;
    }
  }

  // A muted lane still writes its parameters but plays no notes; muting lets what is sounding go.
  const std::uint8_t mask = static_cast<std::size_t>(tick_) < lane.mask.size() ? lane.mask[static_cast<std::size_t>(tick_)] : 0;
  if (mask & 4) { allOff(lane); std::fill(lane.prev.begin(), lane.prev.end(), -1); }   // a note-off marker
  const bool muted = (mask & 3) > 0;
  if (muted != lane.muted) {
    lane.muted = muted;
    if (muted) { allOff(lane); std::fill(lane.prev.begin(), lane.prev.end(), -1); }
  }
  if (muted) return;

  for (std::size_t k = 0; k < lane.trigs.size(); k++) {   // a trigger cell is a hit of its own: velocity is the digit, 0 is nothing
    const double vel = decodeHex1(lane.trigs[k]) / 15.0;
    if (lane.trigs[k] == kEmpty || vel <= 0) continue;
    NoteEvent on;
    on.note = lane.trigCols[k]->note;
    on.vel = vel;
    runtime_.note(lane.trigCols[k]->target.empty() ? lane.instrument : lane.trigCols[k]->target, on);
  }
  for (int k = 0; k < lane.noteCols; k++) {
    const std::size_t column = static_cast<std::size_t>(k);
    const float note = lane.notes[column];
    if (note == kEmpty) continue;
    if (note == kCut) {
      runtime_.kill(lane.noteTargets[column]);   // ^^^ is a kill: a fade out, a reset, then the machine sleeps until its next note
      if (lane.targets.empty()) std::fill(lane.prev.begin(), lane.prev.end(), -1);
      else for (std::size_t j = 0; j < lane.prev.size(); j++) if (lane.noteTargets[j] == lane.noteTargets[column]) lane.prev[j] = -1;
      continue;
    }
    // A note off ends the column's last note. So does a new note, if the last one has not been released yet: it is
    // released first, and then what happens to it is up to the instrument (a one-shot sample plays out, a looping one
    // fades; a polyphonic instrument lets it ring through its release while the new note takes another voice).
    if (note == kOff) { release(lane, k); continue; }
    const int from = lane.prev[column];   // still sounding in this column, or -1
    release(lane, k, true);
    NoteEvent on;
    on.from = from;
    on.column = lane.columnKeys[column];
    on.note = decodeNote(note);
    on.vel = lane.vels[column];
    runtime_.note(lane.noteTargets[column], on);
    lane.prev[column] = on.note;
  }
}

void Sequencer::fireTick() {
  if (once_ && wrapped_) { stop(); return; }   // the last row has had its full length
  runtime_.clock().tickId++;   // nodes that count song ticks (the MOD channel) watch this
  for (auto& lane : lanes_) playRow(*lane);
  // (the Node Sequencer's tick goes here, after the patterns have written their parameters, once it is ported)
  if (std::abs(songPos_ - tick_) > 1.5) songPos_ = tick_;   // the end of the song: back on the grid
  if (++tick_ >= song_.length) tick_ = 0;
  if (once_ && tick_ == 0) wrapped_ = true;
}

void Sequencer::process(float* left, float* right, int n) {
  Tempo now = tempo();
  double samplesPerTick = sampleRate_ * 60 / (now.bpm * now.tpb);
  int group = std::max(1, jsRoundInt(now.tpb / 4.0));
  int done = 0;
  while (done < n) {
    int span = n - done;
    if (playing_) {
      if (countdown_ <= 0) {
        // Swing: ticks come in pairs, and the first of a pair lasts `swing` of the pair, so the second (the off-beat)
        // lands late. 0.5 is straight; 2/3 is a triplet shuffle. At finer ticks the swing stays on the 16ths: with 8
        // ticks to a beat the groups are two ticks long, so changing the resolution does not change the groove.
        const bool first = ((tick_ / group) & 1) == 0;
        fireTick();
        now = tempo();   // the row may have changed the tempo
        samplesPerTick = sampleRate_ * 60 / (now.bpm * now.tpb);
        group = std::max(1, jsRoundInt(now.tpb / 4.0));
        tickLen_ = 2 * samplesPerTick * (first ? now.swing : 1 - now.swing);
        countdown_ += tickLen_;
      }
      span = std::min(span, std::max(1, static_cast<int>(std::ceil(countdown_))));   // (also in the pass where the song has just stopped, as the JS does)
    }
    Clock& clock = runtime_.clock();
    clock.playing = playing_;
    clock.songPos = songPos_;
    clock.tickSamples = samplesPerTick;
    // (the lane curves and the Node Sequencer's control signals are applied here, once they are ported)
    runtime_.process(scratchLeft_.data(), scratchRight_.data(), span);

    const MasterSlots& m = song_.master;
    if (m.present()) {   // the master volume, eased across the span
      const double to = params_[m.volume], from = gainSet_ ? gain_ : to;
      if (from != 1 || to != 1) {
        for (int i = 0; i < span; i++) {
          const double g = from + (to - from) * (i + 1) / span;
          scratchLeft_[static_cast<std::size_t>(i)] = static_cast<float>(scratchLeft_[static_cast<std::size_t>(i)] * g);
          scratchRight_[static_cast<std::size_t>(i)] = static_cast<float>(scratchRight_[static_cast<std::size_t>(i)] * g);
        }
      }
      gain_ = to;
      gainSet_ = true;
      // the limiter, after the volume
      if (m.limit >= 0 && params_[m.limit] >= 0.5f) {
        limiter_.process(scratchLeft_.data(), scratchRight_.data(), span, params_[m.ceiling], params_[m.boost], params_[m.release]);
      }
    }
    if (playing_) songPos_ += span / samplesPerTick;
    std::copy(scratchLeft_.begin(), scratchLeft_.begin() + span, left + done);
    std::copy(scratchRight_.begin(), scratchRight_.begin() + span, right + done);
    if (playing_) countdown_ -= span;
    done += span;
  }
}

}  // namespace wax
