// The bundled wavetable set: 256 single-cycle waves of 128 unsigned 8-bit samples.
#pragma once

namespace wax {

constexpr int kTableWaves = 256, kTablePoints = 128;
extern const unsigned char kTableBytes[kTableWaves * kTablePoints];

}  // namespace wax
