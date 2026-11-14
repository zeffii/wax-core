#include "io/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace wax {
namespace {

// WAV files are little-endian; these read and write byte by byte so the code does not care what the machine is.
std::uint32_t readU32(const unsigned char* at) { return at[0] | (at[1] << 8) | (at[2] << 16) | (static_cast<std::uint32_t>(at[3]) << 24); }
std::uint16_t readU16(const unsigned char* at) { return static_cast<std::uint16_t>(at[0] | (at[1] << 8)); }

void putU32(std::vector<unsigned char>& out, std::uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(static_cast<unsigned char>(v >> (8 * i))); }
void putU16(std::vector<unsigned char>& out, std::uint16_t v) { for (int i = 0; i < 2; i++) out.push_back(static_cast<unsigned char>(v >> (8 * i))); }
void putTag(std::vector<unsigned char>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

float readSample(const unsigned char* at, int format, int bits) {
  if (format == 3 && bits == 32) {
    const std::uint32_t raw = readU32(at);
    float value;
    std::memcpy(&value, &raw, 4);
    return value;
  }
  if (bits == 16) return static_cast<std::int16_t>(readU16(at)) / 32767.0f;
  if (bits == 24) {
    std::int32_t value = at[0] | (at[1] << 8) | (at[2] << 16);
    if (value & 0x800000) value -= 0x1000000;
    return value / 8388607.0f;
  }
  throw std::runtime_error("this reads 16-bit, 24-bit and 32-bit float WAV files only");
}

double toDb(double x) { return 20.0 * std::log10(std::max(x, 1e-15)); }

}  // namespace

StereoAudio readWav(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot open \"" + path + "\".");
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    throw std::runtime_error("\"" + path + "\" is not a WAV file.");
  }

  StereoAudio audio;
  int format = 0, channels = 0, bits = 0;
  // A WAV is a list of chunks: a four-letter name, a length, then that many bytes (padded to an even count).
  for (std::size_t at = 12; at + 8 <= bytes.size();) {
    const unsigned char* chunk = bytes.data() + at;
    const std::size_t size = std::min<std::size_t>(readU32(chunk + 4), bytes.size() - at - 8);
    const unsigned char* body = chunk + 8;
    if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
      format = readU16(body);
      channels = readU16(body + 2);
      audio.sampleRate = static_cast<int>(readU32(body + 4));
      bits = readU16(body + 14);
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      if (channels < 1 || channels > 2) throw std::runtime_error("\"" + path + "\" is neither mono nor stereo.");
      const std::size_t bytesPerSample = static_cast<std::size_t>(bits / 8);
      const std::size_t frames = size / (bytesPerSample * static_cast<std::size_t>(channels));
      audio.left.resize(frames);
      audio.right.resize(frames);
      for (std::size_t f = 0; f < frames; f++) {
        const unsigned char* frame = body + f * bytesPerSample * static_cast<std::size_t>(channels);
        audio.left[f] = readSample(frame, format, bits);
        audio.right[f] = channels == 2 ? readSample(frame + bytesPerSample, format, bits) : audio.left[f];
      }
      return audio;
    }
    at += 8 + size + (size & 1);
  }
  throw std::runtime_error("\"" + path + "\" holds no sound.");
}

void writeWav(const std::string& path, const StereoAudio& audio) {
  const std::size_t frames = audio.frames();
  const std::uint32_t dataSize = static_cast<std::uint32_t>(frames * 8);
  std::vector<unsigned char> out;
  out.reserve(44 + dataSize);
  putTag(out, "RIFF"); putU32(out, 36 + dataSize); putTag(out, "WAVE");
  putTag(out, "fmt "); putU32(out, 16);
  putU16(out, 3);                                                    // format 3: floating point
  putU16(out, 2);                                                    // channels
  putU32(out, static_cast<std::uint32_t>(audio.sampleRate));
  putU32(out, static_cast<std::uint32_t>(audio.sampleRate) * 8);     // bytes per second
  putU16(out, 8);                                                    // bytes per frame
  putU16(out, 32);                                                   // bits per sample
  putTag(out, "data"); putU32(out, dataSize);
  for (std::size_t f = 0; f < frames; f++) {
    for (const float sample : {audio.left[f], audio.right[f]}) {
      std::uint32_t raw;
      std::memcpy(&raw, &sample, 4);
      putU32(out, raw);
    }
  }
  std::ofstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot write \"" + path + "\".");
  file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

AudioDifference compareAudio(const StereoAudio& a, const StereoAudio& b) {
  AudioDifference result;
  result.frames = std::min(a.frames(), b.frames());
  result.lengthGap = std::max(a.frames(), b.frames()) - result.frames;
  double worst = 0, sumOfSquares = 0;
  for (std::size_t f = 0; f < result.frames; f++) {
    const double dl = static_cast<double>(a.left[f]) - b.left[f];
    const double dr = static_cast<double>(a.right[f]) - b.right[f];
    const double here = std::max(std::abs(dl), std::abs(dr));
    if (here > worst) { worst = here; result.worstFrame = f; }
    sumOfSquares += dl * dl + dr * dr;
  }
  result.peakDb = toDb(worst);
  result.rmsDb = result.frames ? toDb(std::sqrt(sumOfSquares / (2.0 * static_cast<double>(result.frames)))) : -300;
  return result;
}

}  // namespace wax
