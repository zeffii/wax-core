#include "engine/offline.h"
#include "project/samples.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/js_math.h"
#include "core/node_registry.h"
#include "engine/runtime.h"
#include "engine/sequencer.h"
#include "graph/compile.h"
#include "engine/engine.h"

namespace wax {

RenderResult renderSong(const Project& project, const RenderOptions& options) {
  if (project.song.length <= 0) throw std::runtime_error("There is no song to render.");
  constexpr int kBlock = kEngineBlock;
  Engine engine(project, options.sampleRate, options.threads);
  GraphRuntime& runtime = engine.runtime();
  Sequencer& sequencer = engine.sequencer();
  RenderResult result;
  result.warnings = engine.warnings();

  // With delay compensation on, the whole mix comes out late by `lag` samples: the start is trimmed off below.
  const int lag = runtime.alignment() + runtime.carveLatency();
  const std::size_t limit = static_cast<std::size_t>(std::floor(options.sampleRate * options.maxSeconds));
  double tailLeft = jsRound(options.tailSeconds * options.sampleRate) + lag;
  std::vector<float> left, right;
  float blockLeft[kBlock], blockRight[kBlock];
  sequencer.play(0, true);
  while (left.size() < limit) {
    if (!sequencer.playing() && tailLeft <= 0) break;
    sequencer.process(blockLeft, blockRight, kBlock);
    if (!sequencer.playing()) tailLeft -= kBlock;
    for (int i = 0; i < kBlock; i++) result.peak = std::max({result.peak, static_cast<double>(std::abs(blockLeft[i])), static_cast<double>(std::abs(blockRight[i]))});
    left.insert(left.end(), blockLeft, blockLeft + kBlock);
    right.insert(right.end(), blockRight, blockRight + kBlock);
  }
  if (runtime.sawUnported()) result.warnings.push_back("the mixer's carve stage is not ported yet: the song switches it on");

  const std::size_t skip = std::min(static_cast<std::size_t>(lag), left.size());
  result.audio.sampleRate = static_cast<int>(options.sampleRate);
  result.audio.left.assign(left.begin() + static_cast<std::ptrdiff_t>(skip), left.end());
  result.audio.right.assign(right.begin() + static_cast<std::ptrdiff_t>(skip), right.end());
  return result;
}

}  // namespace wax
