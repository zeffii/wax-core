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

    registerCanyon(r);
    registerChord(r);
    registerChorus(r);
    registerClap(r);
    registerCombPerc(r);
    registerControlNodes(r);
    registerDelay(r);
    registerDuette(r);
    registerEnv(r);
    registerGain(r);
    registerHall(r);
    registerHat808(r);
    registerLfo(r);
    registerFilter(r);
    registerNoise(r);
    registerCrusher(r);
    registerGater(r);
    registerMultiTap(r);
    registerFreqShift(r);
    registerPhaser(r);
    registerShimmer(r);
    registerFmSynth(r);
    registerPiano(r);
    registerTape(r);
    registerString(r);
    registerWavetable(r);
    registerAddSynth(r);
    registerArp(r);
    registerSpecPan(r);
    registerBlur(r);
    registerShiva(r);
    registerWoodblock(r);
    registerCompressor(r);
    registerFlanger(r);
    registerFilterPro(r);
    registerMix(r);
    registerDcBlock(r);
    registerDistortion(r);
    registerMosh(r);
    registerOsc(r);
    registerPlate(r);
    registerPluckGate(r);
    registerSidechain(r);
    registerSquelch(r);
    registerSubSynth(r);
    registerVocoder(r);
    registerWaveFolder(r);
    registerWestCoast(r);
    return r;
  }();
  return registry;
}

}  // namespace wax
