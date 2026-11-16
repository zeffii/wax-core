// A spectrogram of a sample for the Sample screen's display: time runs left to right (on the waveform's axis), low frequencies at the
// bottom. Port of webtracker src/project/spectrogram.js.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace wax {

// In-place radix-2 FFT of re/im (their length a power of two, the same).
void fft(std::vector<double>& re, std::vector<double>& im);

// Brightness 0..255 for cols x rows cells (row 0 at the top) of the frames [from, to) of the sample. Frequency is spread so the low end gets
// more rows (a square-root scale); brightness is the level in dB below the loudest cell over a 90 dB range. `right` (empty for mono) is averaged in.
std::vector<std::uint8_t> computeSpectrogram(const std::vector<float>& left, const std::vector<float>& right, int from, int to, int cols, int rows);

// The colour of a brightness: black, through blue and magenta, to near white (r, g, b, a).
std::array<std::uint8_t, 4> spectrogramHeat(int brightness);

}  // namespace wax
