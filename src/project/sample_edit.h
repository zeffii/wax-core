// Editing the audio of a sample: each tool changes a range of it (or all of it) and keeps its loop points sensible.
// Port of webtracker src/project/sampleedit.js. A range is [from, to) in frames; the whole sample when it is empty.
// The tools work on an EditableSample, the decoded form of a sample of the project (see readSample / writeSample).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/node.h"   // Json

namespace wax {

struct SampleRange {
  int from = 0, to = 0;
};

// A sample as it is edited: float audio, and the properties that go with it (loops, sustain loop, levels, slices), as the file keeps them.
struct EditableSample {
  double rate = 44100;
  std::vector<float> left, right;   // `right` empty: a mono sample
  Json props = Json::object();      // loopStart, loopEnd, loopOn, loopXfade, susStart..., volume, globalVolume, pan, slices, roll

  bool stereo() const { return !right.empty(); }
  int frames() const { return static_cast<int>(left.size()); }
  double number(const char* key, double fallback = 0) const { return props.contains(key) && props[key].is_number() ? props[key].get<double>() : fallback; }
};

// A sample of the project's `samples` object, decoded (16-bit PCM or float32); and back. A sample is kept as 16-bit PCM, like the file.
EditableSample readSample(const Json& json);
Json writeSample(const EditableSample& sample, const Json& original = Json::object());   // `original`: the sample as it was, whose other keys are kept

// Every tool throws std::runtime_error with a message the screen shows when it cannot do what was asked (an empty range, a silent range...).
// A clip: audio taken from a sample for pasting.
struct SampleClip {
  std::vector<float> left, right;
};
SampleClip copyRange(const EditableSample& sample, std::optional<SampleRange> range);

enum class PasteMode { Insert, Overwrite };
// Puts a clip in at frame `at`. Insert opens a gap for it (loops, slice markers and the sustain loop after that point move along; a loop that
// surrounds it grows); Overwrite replaces the frames it lands on (growing the end when it runs past). A mono clip into a stereo sample goes
// to both sides; a stereo clip into a mono sample is mixed down.
void pasteClip(EditableSample& sample, int at, const SampleClip& clip, PasteMode mode);

void cropSample(EditableSample& sample, std::optional<SampleRange> range);   // keep only the range; with none, the loop region
void cutSample(EditableSample& sample, SampleRange range);                   // remove the range
void silenceSample(EditableSample& sample, std::optional<SampleRange> range);
void reverseSample(EditableSample& sample, std::optional<SampleRange> range);
void normaliseSample(EditableSample& sample, std::optional<SampleRange> range);   // so the loudest frame of either channel reaches full scale
void gainSample(EditableSample& sample, std::optional<SampleRange> range, double factor);
void fadeInSample(EditableSample& sample, std::optional<SampleRange> range);
void fadeOutSample(EditableSample& sample, std::optional<SampleRange> range);
void removeDcSample(EditableSample& sample, std::optional<SampleRange> range);   // take the average out
void bakeCrossfades(EditableSample& sample);   // write each loop's crossfade into the audio, the way the sampler plays it live, and clear the setting

// A tool by name, for the screen's keys and the undo label: "crop", "cut", "silence", "reverse", "normalise", "fadeIn", "fadeOut", "removeDc", "bakeCrossfade".
// The settings and markers (all throw std::runtime_error with a message for a value that makes no sense):
// A loop in frames; start and end both 0 take it away. A new loop starts switched off, like the web app's.
void setLoopPoints(EditableSample& sample, int start, int end, bool sustain);
// The slice markers: frames inside the sample, kept sorted, without doubles.
void setSliceMarkers(EditableSample& sample, std::vector<int> frames);
std::vector<int> sliceMarkers(const EditableSample& sample);
// A level: "volume", "globalVolume" (0..1, default 1) or "pan" (-1..1, default 0). A default is not stored.
void setSampleLevel(EditableSample& sample, const std::string& key, double value);
// Whether a loop plays: it has points, and is not switched off.
bool loopActive(const EditableSample& sample, bool sustain);

void applySampleTool(EditableSample& sample, const std::string& tool, std::optional<SampleRange> range);

}  // namespace wax
