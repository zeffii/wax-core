#include "nodes/all_nodes.h"

namespace wax {

const NodeRegistry& builtinNodes() {
  static const NodeRegistry registry = [] {
    NodeRegistry r;
    registerPlumbing(r);
    registerOut(r);
    registerBusIn(r);
    registerTempo(r);
    registerVoices(r);

    registerClap(r);
    registerCombPerc(r);
    registerDuette(r);
    registerEnv(r);
    registerGain(r);
    registerHat808(r);
    registerOsc(r);
    registerSquelch(r);
    registerSubSynth(r);
    return r;
  }();
  return registry;
}

}  // namespace wax
