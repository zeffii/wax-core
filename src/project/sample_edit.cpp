#include "project/sample_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "project/samples.h"

namespace wax {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The two loops of a sample: its key names (start, end, on, crossfade).
struct LoopKeys { const char *start, *end, *on, *xfade; };
const LoopKeys kLoops[] = {{"loopStart", "loopEnd", "loopOn", "loopXfade"}, {"susStart", "susEnd", "susOn", "susXfade"}};

std::vector<std::vector<float>*> channelsOf(EditableSample& s) {
  std::vector<std::vector<float>*> out{&s.left};
  if (s.stereo()) out.push_back(&s.right);
  return out;
}

SampleRange clampRange(const EditableSample& s, const std::optional<SampleRange>& range) {
  const int n = s.frames();
  const int from = std::max(0, std::min(n, range ? range->from : 0)), to = std::max(0, std::min(n, range ? range->to : n));
  if (to - from < 1) throw std::runtime_error("Nothing to edit: the range is empty.");
  return {from, to};
}

void dropLoop(EditableSample& s, const LoopKeys& keys) {
  for (const char* key : {keys.start, keys.end, keys.on, keys.xfade}) s.props.erase(key);
}

// Moves the loops after the audio changes length: map(point) says where an old frame lands. A loop left shorter than two frames is removed.
template <class Map>
void remapLoops(EditableSample& s, Map map) {
  for (const LoopKeys& keys : kLoops) {
    if (!(s.number(keys.end) > 0)) continue;
    const double a = map(s.number(keys.start)), b = map(s.number(keys.end));
    if (b - a < 2 || b > s.frames()) dropLoop(s, keys);
    else { s.props[keys.start] = a; s.props[keys.end] = b; }
  }
}

std::vector<double> slicesOf(const EditableSample& s) {
  std::vector<double> out;
  if (s.props.contains("slices") && s.props["slices"].is_array()) for (const Json& v : s.props["slices"]) if (v.is_number()) out.push_back(v.get<double>());
  return out;
}

void setSlices(EditableSample& s, std::vector<double> slices) {
  std::sort(slices.begin(), slices.end());
  slices.erase(std::unique(slices.begin(), slices.end()), slices.end());
  if (slices.empty()) s.props.erase("slices"); else s.props["slices"] = slices;
}

const char* const kPropKeys[] = {"loopStart", "loopEnd", "loopOn", "loopXfade", "susStart", "susEnd", "susOn", "susXfade", "pingpong", "volume", "globalVolume", "pan", "slices", "roll"};

}  // namespace

EditableSample readSample(const Json& json) {
  EditableSample s;
  s.rate = json.value("rate", 44100.0);
  const std::string encoding = json.value("encoding", std::string("pcm16"));
  if (json.contains("data") && json["data"].is_string()) s.left = decodeSampleChannel(json["data"].get<std::string>(), encoding);
  if (json.contains("right") && json["right"].is_string()) s.right = decodeSampleChannel(json["right"].get<std::string>(), encoding);
  for (const char* key : kPropKeys) if (json.contains(key)) s.props[key] = json[key];
  return s;
}

Json writeSample(const EditableSample& s, const Json& original) {
  Json out = original.is_object() ? original : Json::object();
  const Json audio = encodeSample(s.rate, s.left, s.right);
  out["rate"] = audio["rate"];
  out["encoding"] = audio["encoding"];
  out["data"] = audio["data"];
  if (audio.contains("right")) out["right"] = audio["right"]; else out.erase("right");
  for (const char* key : kPropKeys) {
    if (s.props.contains(key)) out[key] = s.props[key]; else out.erase(key);
  }
  return out;
}

SampleClip copyRange(const EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  SampleClip clip;
  clip.left.assign(s.left.begin() + r.from, s.left.begin() + r.to);
  if (s.stereo()) clip.right.assign(s.right.begin() + r.from, s.right.begin() + r.to);
  return clip;
}

void pasteClip(EditableSample& s, int at, const SampleClip& clip, PasteMode mode) {
  const int n = s.frames(), len = static_cast<int>(clip.left.size());
  if (len == 0) throw std::runtime_error("The clipboard is empty.");
  at = std::max(0, std::min(n, at));
  std::vector<float> mono;
  if (!s.stereo() && !clip.right.empty()) { mono.resize(clip.left.size()); for (std::size_t i = 0; i < mono.size(); i++) mono[i] = (clip.left[i] + clip.right[i]) / 2; }
  const std::vector<float>& leftPiece = !s.stereo() && !clip.right.empty() ? mono : clip.left;
  const std::vector<float>& rightPiece = clip.right.empty() ? clip.left : clip.right;
  const auto make = [&](const std::vector<float>& source, const std::vector<float>& piece) {
    const int end = mode == PasteMode::Insert ? n + len : std::max(n, at + len);
    std::vector<float> out(static_cast<std::size_t>(end), 0.0f);
    std::copy(source.begin(), source.begin() + at, out.begin());
    std::copy(piece.begin(), piece.end(), out.begin() + at);
    if (mode == PasteMode::Insert) std::copy(source.begin() + at, source.end(), out.begin() + at + len);
    else if (at + len < n) std::copy(source.begin() + at + len, source.end(), out.begin() + at + len);
    return out;
  };
  const bool stereo = s.stereo();
  std::vector<float> newLeft = make(s.left, leftPiece);
  std::vector<float> newRight = stereo ? make(s.right, rightPiece) : std::vector<float>();
  s.left = std::move(newLeft);
  s.right = std::move(newRight);
  if (mode == PasteMode::Insert) {
    remapLoops(s, [&](double p) { return p < at ? p : p + len; });
    std::vector<double> slices = slicesOf(s);
    for (double& p : slices) if (p >= at) p += len;
    if (!slices.empty()) setSlices(s, slices);
  }
}

void cropSample(EditableSample& s, std::optional<SampleRange> range) {
  if (!range) {
    if (!(s.number("loopEnd") > 0)) throw std::runtime_error("Select the part to keep first (or set a loop).");
    range = SampleRange{static_cast<int>(s.number("loopStart")), static_cast<int>(s.number("loopEnd"))};
  }
  const SampleRange r = clampRange(s, range);
  for (const LoopKeys& keys : kLoops)
    if (s.number(keys.end) > 0 && !(s.number(keys.start) >= r.from && s.number(keys.end) <= r.to)) dropLoop(s, keys);
  s.left = std::vector<float>(s.left.begin() + r.from, s.left.begin() + r.to);
  if (s.stereo()) s.right = std::vector<float>(s.right.begin() + r.from, s.right.begin() + r.to);
  remapLoops(s, [&](double p) { return p - r.from; });
  std::vector<double> kept;
  for (const double p : slicesOf(s)) if (p >= r.from && p < r.to) kept.push_back(p - r.from);
  setSlices(s, kept);
}

void cutSample(EditableSample& s, SampleRange range) {
  const SampleRange r = clampRange(s, range);
  const int gone = r.to - r.from;
  if (gone >= s.frames()) throw std::runtime_error("That would remove the whole sample.");
  const auto keep = [&](std::vector<float>& a) { a.erase(a.begin() + r.from, a.begin() + r.to); };
  keep(s.left);
  if (s.stereo()) keep(s.right);
  remapLoops(s, [&](double p) { return p <= r.from ? p : p >= r.to ? p - gone : static_cast<double>(r.from); });
  std::vector<double> kept;
  for (const double p : slicesOf(s)) if (p < r.from || p >= r.to) kept.push_back(p >= r.to ? p - gone : p);
  setSlices(s, kept);
}

void silenceSample(EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  for (std::vector<float>* c : channelsOf(s)) std::fill(c->begin() + r.from, c->begin() + r.to, 0.0f);
}

void reverseSample(EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  for (std::vector<float>* c : channelsOf(s)) std::reverse(c->begin() + r.from, c->begin() + r.to);
}

void normaliseSample(EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  float peak = 0;
  for (std::vector<float>* c : channelsOf(s)) for (int i = r.from; i < r.to; i++) peak = std::max(peak, std::abs((*c)[static_cast<std::size_t>(i)]));
  if (peak == 0) throw std::runtime_error("The range is silent.");
  for (std::vector<float>* c : channelsOf(s)) for (int i = r.from; i < r.to; i++) (*c)[static_cast<std::size_t>(i)] /= peak;
}

void gainSample(EditableSample& s, std::optional<SampleRange> range, double factor) {
  const SampleRange r = clampRange(s, range);
  for (std::vector<float>* c : channelsOf(s)) for (int i = r.from; i < r.to; i++) (*c)[static_cast<std::size_t>(i)] = static_cast<float>((*c)[static_cast<std::size_t>(i)] * factor);
}

void fadeInSample(EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  for (std::vector<float>* c : channelsOf(s))
    for (int i = r.from; i < r.to; i++) (*c)[static_cast<std::size_t>(i)] = static_cast<float>((*c)[static_cast<std::size_t>(i)] * (static_cast<double>(i - r.from) / (r.to - r.from)));
}

void fadeOutSample(EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  for (std::vector<float>* c : channelsOf(s))
    for (int i = r.from; i < r.to; i++) (*c)[static_cast<std::size_t>(i)] = static_cast<float>((*c)[static_cast<std::size_t>(i)] * (1 - static_cast<double>(i - r.from) / (r.to - r.from)));
}

void removeDcSample(EditableSample& s, std::optional<SampleRange> range) {
  const SampleRange r = clampRange(s, range);
  for (std::vector<float>* c : channelsOf(s)) {
    double sum = 0;
    for (int i = r.from; i < r.to; i++) sum += (*c)[static_cast<std::size_t>(i)];
    const double mean = sum / (r.to - r.from);
    for (int i = r.from; i < r.to; i++) (*c)[static_cast<std::size_t>(i)] = static_cast<float>((*c)[static_cast<std::size_t>(i)] - mean);
  }
}

void bakeCrossfades(EditableSample& s) {
  int baked = 0;
  for (const LoopKeys& keys : kLoops) {
    const int ls = static_cast<int>(s.number(keys.start)), le = static_cast<int>(s.number(keys.end));
    const int xf = le > 0 ? std::max(0, std::min({static_cast<int>(s.number(keys.xfade)), ls, le - ls})) : 0;
    if (!xf) continue;
    const int span = le - ls;
    for (std::vector<float>* c : channelsOf(s)) {
      const std::vector<float> original = *c;
      for (int i = le - xf; i < le; i++) {
        const double t = static_cast<double>(i - (le - xf)) / xf;
        (*c)[static_cast<std::size_t>(i)] = static_cast<float>(original[static_cast<std::size_t>(i)] * std::cos(t * kPi / 2) + original[static_cast<std::size_t>(i - span)] * std::sin(t * kPi / 2));
      }
    }
    s.props.erase(keys.xfade);
    baked++;
  }
  if (!baked) throw std::runtime_error("No loop has a crossfade to bake.");
}

void setLoopPoints(EditableSample& s, int start, int end, bool sustain) {
  const LoopKeys& keys = kLoops[sustain ? 1 : 0];
  if (start < 0 || end < 0) throw std::runtime_error("Loop points are whole numbers of frames.");
  if (start == 0 && end == 0) { dropLoop(s, keys); return; }
  if (end > s.frames()) throw std::runtime_error("The loop end is past the end of the sample (" + std::to_string(s.frames()) + " frames).");
  if (end - start < 2) throw std::runtime_error("The loop must be at least two frames long.");
  s.props[keys.start] = start;
  s.props[keys.end] = end;
  if (!s.props.contains(keys.on)) s.props[keys.on] = false;
}

std::vector<int> sliceMarkers(const EditableSample& s) {
  std::vector<int> out;
  for (const double p : slicesOf(s)) out.push_back(static_cast<int>(p));
  return out;
}

void setSliceMarkers(EditableSample& s, std::vector<int> frames) {
  for (const int f : frames)
    if (f < 0 || f >= s.frames()) throw std::runtime_error("Slice markers are whole frames inside the sample (0 to " + std::to_string(s.frames() - 1) + ").");
  setSlices(s, std::vector<double>(frames.begin(), frames.end()));
}

void setSampleLevel(EditableSample& s, const std::string& key, double value) {
  const bool pan = key == "pan";
  if (!pan && key != "volume" && key != "globalVolume") throw std::runtime_error("Unknown sample setting: " + key);
  const double lo = pan ? -1 : 0;
  if (!(value >= lo && value <= 1)) throw std::runtime_error(key + " must be between " + (pan ? "-1" : "0") + " and 1.");
  const double def = pan ? 0 : 1;
  if (value == def) s.props.erase(key); else s.props[key] = value;
}

bool loopActive(const EditableSample& s, bool sustain) {
  const LoopKeys& keys = kLoops[sustain ? 1 : 0];
  return s.number(keys.end) > 0 && !(s.props.contains(keys.on) && s.props[keys.on].is_boolean() && !s.props[keys.on].get<bool>());
}

void applySampleTool(EditableSample& s, const std::string& tool, std::optional<SampleRange> range) {
  if (tool == "crop") cropSample(s, range);
  else if (tool == "cut") { if (!range) throw std::runtime_error("Select the part to cut first."); cutSample(s, *range); }
  else if (tool == "silence") silenceSample(s, range);
  else if (tool == "reverse") reverseSample(s, range);
  else if (tool == "normalise") normaliseSample(s, range);
  else if (tool == "fadeIn") fadeInSample(s, range);
  else if (tool == "fadeOut") fadeOutSample(s, range);
  else if (tool == "removeDc") removeDcSample(s, range);
  else if (tool == "bakeCrossfade") bakeCrossfades(s);
  else throw std::runtime_error("Unknown edit: " + tool);
}

}  // namespace wax
