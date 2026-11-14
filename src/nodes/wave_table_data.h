// The bundled wavetable set: 256 single-cycle waves of 128 unsigned 8-bit samples.
#pragma once

#include <vector>

namespace wax {

constexpr int kTableWaves = 256, kTablePoints = 128;
extern const unsigned char kTableBytes[kTableWaves * kTablePoints];

// The 256 frames of 512 points the web app builds from them (cubic resample, settled), made once and shared by every node.
constexpr int kTableFrame = 512;
const std::vector<float>& bundledWaveFrames();

}  // namespace wax
