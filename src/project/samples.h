// The samples of a project: decoded from the file, and what each sampler node is sent for them.
// Port of decodeSample (webtracker src/project/file.js), ensurePool and syncSamplerStereo (src/project/ops.js).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "project/project.h"

namespace wax {

// Every sample of the project by id, decoded (16-bit PCM or float32, base64 in the file), with the tap pool's sixteen
// one-frame silent slots (tap01 to tap16) added when the file does not have them.
std::map<std::string, std::shared_ptr<const SampleMessage>> decodeSamples(const Json& samples);

// The sample ids an instrument's node plays, in slot order ("samples": { node: id | [ids] }).
std::vector<std::string> sampleSlots(const Instrument& instrument, const std::string& nodeId);

// A copy of the instrument's graph where each sampler node is `stereo` when any of its samples has two channels.
Graph graphWithSamplerStereo(const Instrument& instrument, const std::map<std::string, std::shared_ptr<const SampleMessage>>& samples);

// The tap pool's slots (tap01 to tap16): not samples of the user's.
bool isPoolSample(const std::string& id);

// A file name made into a sample id: no extension, letters, digits, _ and - only, at most 24 characters (a name of digits only gets a leading _).
std::string sampleIdFromName(const std::string& fileName);

// The id for a new sample: `base`, then base2, base3 ... the first the project does not have.
std::string uniqueSampleId(const Json& samples, const std::string& base);

// One channel of a sample of the project file: base64 of 16-bit PCM ("pcm16") or of float32, as `encoding` says.
std::vector<float> decodeSampleChannel(const std::string& base64, const std::string& encoding);

// A sample as the project file keeps it: 16-bit PCM, base64. `right` empty: a mono sample.
Json encodeSample(double rate, const std::vector<float>& left, const std::vector<float>& right);

}  // namespace wax
