// Machine sleep: a machine that has gone quiet stops costing anything until something could make it sound again.
// Port of webtracker src/audio/sleep.js (the long comment there says when a machine sleeps and what wakes it).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/node.h"
#include "graph/compile.h"

namespace wax {

inline constexpr double kSilent = 3e-5;   // about -90 dB

// What the runtime keeps per machine for sleeping.
struct SleepState {
  bool asleep = false;
  int quiet = 0;                        // samples of silence so far
  std::vector<int> watched;             // the parameter slots that wake it
  std::vector<float> values;            // their values when it fell asleep
  std::vector<std::uint32_t> stamps;    // and how often a pattern had written them
  std::vector<int> times;               // the slots holding a delay time, in ms
  std::vector<std::string> sources;     // the machines its sidechains listen to
  bool woken = false;                   // a note or message came in since the last block
};

// Made when a machine is loaded. Nothing for a machine that never sleeps.
std::optional<SleepState> makeSleepState(const CompiledGraph& compiled);

// Whether a node type is a modulator that runs with no note (lfo, ctrl): those keep time while their machine sleeps.
bool isFreeRunning(const std::string& type);

// The silence a machine needs before it sleeps, in samples: two seconds, or its longest delay time plus a margin.
int holdSamples(const SleepState& z, const float* params, double sampleRate);

// Goes to sleep: remembers the parameters as they are now.
void fallAsleep(SleepState& z, const float* params, const Clock& clock);

// Whether a sleeping machine has a reason to wake this block. peakOf(id): the last peak of another machine, or 0.
bool shouldWake(const SleepState& z, const float* params, const Clock& clock, const std::function<double(const std::string&)>& peakOf);

}  // namespace wax
