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

}  // namespace wax
