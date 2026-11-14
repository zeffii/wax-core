// Turning what the tap nodes record into samples. Port of the take handling in webtracker src/main.js (engine.onEmit,
// store, fade): the chunks of a take are joined, the ends are faded with a raised cosine, and the result belongs in
// the tap pool slot (tap01 to tap16) and, when the node asks, in a normal sample as well.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/runtime.h"

namespace wax {

struct FinishedTake {
  std::string instrument, node;
  int smp = 1;       // the pool slot, 1 to 16
  int copy = 0;      // a normal sample number that also receives a copy, 0 for none
  double rate = 44100;
  std::vector<float> left, right;   // right stays empty for one channel; already faded
};

std::string tapPoolId(int smp);   // "tap01" ... "tap16"

// The sample a finished take makes: whole-length loop markers, as the web app writes them.
std::shared_ptr<SampleMessage> takeSample(const FinishedTake& take);

class TakeAssembler {
public:
  // Feed the messages of GraphRuntime::drainTakes() in order. A finished take comes back when its End message arrives
  // (a take with no audio is dropped).
  std::optional<FinishedTake> feed(const TakeEvent& event);

private:
  struct Open {
    TakeMessage start;
    std::vector<float> l, r;
  };
  std::map<std::string, Open> open_;
};

}  // namespace wax
