#include "engine/sleep.h"

#include <algorithm>
#include <set>

#include "core/js_math.h"

namespace wax {
namespace {

constexpr double kHoldSeconds = 2;      // the shortest silence before a machine sleeps
constexpr double kMarginSeconds = 0.5;  // added to the longest delay time found

// machines with one of these can start sounding by themselves, so they never sleep
const std::set<std::string> kNever = {"genexpr", "glsl", "tempo", "nsgroups", "oscout", "modchan", "xmchan", "itchan"};
const std::set<std::string> kFree = {"lfo", "ctrl"};
// sound only while a note does
const std::set<std::string> kGated = {
  "env", "env2", "notetrig", "sampler", "drumsynth", "drumsynth2", "clap", "membrane", "clang", "subsynth", "pulsar", "pulsetrain",
  "westcoast", "pluckgate", "fourpole", "oberheim", "cardinal", "piano", "fmsynth", "dvii", "wavetable", "granular", "woodblock",
  "jawharp", "snarl", "string", "hat808", "hat2", "bongo", "combperc", "squelch", "duette", "compass", "waveseq", "khufu", "lone"};
// in ms, on the effect node types that have them
const std::set<std::string> kTimeParams = {"time", "length", "delay", "predelay", "maxdelay", "mindelay", "ringtime"};

bool anyNode(const CompiledGraph& compiled, const std::set<std::string>& types) {
  return std::any_of(compiled.nodes.begin(), compiled.nodes.end(), [&](const CompiledNode& n) { return types.count(n.type) > 0; });
}

}  // namespace

bool isFreeRunning(const std::string& type) { return kFree.count(type) > 0; }

std::optional<SleepState> makeSleepState(const CompiledGraph& compiled) {
  if (!compiled.sleeps) return std::nullopt;
  if (anyNode(compiled, kNever)) return std::nullopt;
  if (anyNode(compiled, kFree) && !anyNode(compiled, kGated)) return std::nullopt;

  SleepState z;
  const auto watch = [&](int slot) {
    if (std::find(z.watched.begin(), z.watched.end(), slot) == z.watched.end()) z.watched.push_back(slot);
  };
  for (const CompiledNode& n : compiled.nodes) {
    for (std::size_t i = 0; i < n.slots.size(); i++) {
      const int slot = n.slots[i];
      if (slot < 0) continue;
      watch(slot);
      const ParamSpec& spec = n.def->params[i];
      if (kTimeParams.count(spec.name) && (spec.unit.empty() || spec.unit == "ms")) z.times.push_back(slot);
    }
    if (n.type == "sidechain" && !n.source.empty()) z.sources.push_back(n.source);
  }
  if (compiled.amp >= 0) watch(compiled.amp);
  if (compiled.send >= 0) watch(compiled.send);
  z.values.assign(z.watched.size(), 0.0f);
  z.stamps.assign(z.watched.size(), 0);
  return z;
}

int holdSamples(const SleepState& z, const float* params, double sampleRate) {
  double longest = 0;
  for (const int slot : z.times) {
    const double ms = params[slot];
    if (ms > longest) longest = ms;   // (a value that is not a number counts as 0)
  }
  return jsRoundInt(std::max(kHoldSeconds, longest / 1000 + kMarginSeconds) * sampleRate);
}

void fallAsleep(SleepState& z, const float* params, const Clock& clock) {
  for (std::size_t i = 0; i < z.watched.size(); i++) {
    z.values[i] = params[z.watched[i]];
    z.stamps[i] = clock.stamps[static_cast<std::size_t>(z.watched[i])];
  }
  z.asleep = true;
  z.woken = false;
}

bool shouldWake(const SleepState& z, const float* params, const Clock& clock, const std::function<double(const std::string&)>& peakOf) {
  if (z.woken) return true;
  for (std::size_t i = 0; i < z.watched.size(); i++) {
    const int slot = z.watched[i];
    if (params[slot] != z.values[i] || clock.stamps[static_cast<std::size_t>(slot)] != z.stamps[i]) return true;
  }
  for (const std::string& id : z.sources) if (peakOf(id) >= kSilent) return true;
  return false;
}

}  // namespace wax
