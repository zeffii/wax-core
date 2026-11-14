// Single-cycle waveforms: kWavePoints values from -1 to 1, one period, read with linear interpolation.
// Port of the parts of webtracker src/audio/nodes/waves.js that the nodes use.
#pragma once

#include <array>

#include "core/node.h"   // Json

namespace wax {

inline constexpr int kWavePoints = 128;

using WaveTable = std::array<float, kWavePoints>;

// Any stored waveform (a JSON list of numbers) becomes kWavePoints values in -1..1; anything unusable is a sine.
WaveTable normalizeWave(const Json* points);

}  // namespace wax
