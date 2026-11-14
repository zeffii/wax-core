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

    registerChord(r);
    registerChorus(r);
    registerClap(r);
    registerCombPerc(r);
    registerControlNodes(r);
    registerDuette(r);
    registerEnv(r);
    registerGain(r);
    registerHall(r);
    registerHat808(r);
    registerOsc(r);
    registerPlate(r);
    registerPluckGate(r);
    registerSquelch(r);
    registerSubSynth(r);
    registerWaveFolder(r);
    registerWestCoast(r);
    return r;
  }();
  return registry;
}

}  // namespace wax
