// The bundled wavetable set: 256 single-cycle waves of 128 unsigned 8-bit samples.
#pragma once

#include <string>
#include <vector>

namespace wax {

constexpr int kTableWaves = 256, kTablePoints = 128;
extern const unsigned char kTableBytes[kTableWaves * kTablePoints];

// The 256 frames of 512 points the web app builds from them (cubic resample, settled), made once and shared by every node.
constexpr int kTableFrame = 512;
const std::vector<float>& bundledWaveFrames();

// What the wavetable node's window needs (wtutil.js): frames kept on the node as base64 of 16-bit numbers, a window or a table cut from a sample.
constexpr int kMaxTableFrames = 128;
std::vector<float> decodeWaveFrames(const std::string& text);            // empty when the text holds nothing
std::string encodeWaveFrames(const std::vector<float>& flat);
std::vector<float> defaultWavePair();                                      // a sine then a saw
std::vector<float> waveWindow(const std::vector<float>& data, int start);  // kTableFrame points cut at `start`, settled
std::vector<float> tableFromSample(const std::vector<float>& data, int size);   // `size` 0: the whole sample as one cycle

}  // namespace wax
