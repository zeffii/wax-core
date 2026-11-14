// One register function per ported node file. Adding a node: write nodes/<name>.cpp with a registerXxx(),
// declare it here, call it in all_nodes.cpp and list the .cpp in CMakeLists.txt.
#pragma once

#include "core/node_registry.h"

namespace wax {

// plumbing the compiler and the runtime rely on
void registerPlumbing(NodeRegistry& registry);   // sum, sumst, fold, compdelay (inserted by the compiler)
void registerOut(NodeRegistry& registry);
void registerBusIn(NodeRegistry& registry);
void registerTempo(NodeRegistry& registry);
void registerVoices(NodeRegistry& registry);

// the machines
void registerCanyon(NodeRegistry& registry);   // prismacanyon
void registerChord(NodeRegistry& registry);
void registerChorus(NodeRegistry& registry);
void registerClap(NodeRegistry& registry);
void registerCombPerc(NodeRegistry& registry);
void registerControlNodes(NodeRegistry& registry);   // random, times
void registerDelay(NodeRegistry& registry);
void registerDuette(NodeRegistry& registry);
void registerEnv(NodeRegistry& registry);
void registerEnv2(NodeRegistry& registry);
void registerHaas(NodeRegistry& registry);
void registerHat2(NodeRegistry& registry);
void registerDrumSynth2(NodeRegistry& registry);
void registerClang(NodeRegistry& registry);
void registerUrugraVerb(NodeRegistry& registry);
void registerSympathy(NodeRegistry& registry);
void registerKaleidoscope(NodeRegistry& registry);
void registerPogue(NodeRegistry& registry);
void registerLone(NodeRegistry& registry);
void registerIslands(NodeRegistry& registry);
void registerGate(NodeRegistry& registry);
void registerLadder(NodeRegistry& registry);
void registerEq(NodeRegistry& registry);
void registerSweep(NodeRegistry& registry);
void registerExciter(NodeRegistry& registry);
void registerCardinal(NodeRegistry& registry);
void registerMembrane(NodeRegistry& registry);
void registerBongo(NodeRegistry& registry);
void registerDrumSynth(NodeRegistry& registry);
void registerSaturator(NodeRegistry& registry);
void registerWaveShaper(NodeRegistry& registry);
void registerTransient(NodeRegistry& registry);
void registerRingMod(NodeRegistry& registry);
void registerComb(NodeRegistry& registry);
void registerGain(NodeRegistry& registry);
void registerHall(NodeRegistry& registry);
std::unique_ptr<Node> makeHall(const NodeContext& context);   // a hall reverb on its own, for the shimmer to run inside
void registerHat808(NodeRegistry& registry);
void registerLfo(NodeRegistry& registry);
void registerMosh(NodeRegistry& registry);
void registerOsc(NodeRegistry& registry);
void registerPlate(NodeRegistry& registry);
void registerPluckGate(NodeRegistry& registry);
void registerSidechain(NodeRegistry& registry);
void registerSquelch(NodeRegistry& registry);
void registerSubSynth(NodeRegistry& registry);
void registerVocoder(NodeRegistry& registry);
void registerWaveFolder(NodeRegistry& registry);
void registerWestCoast(NodeRegistry& registry);
void registerFilter(NodeRegistry& registry);   // filter
void registerNoise(NodeRegistry& registry);   // noise
void registerCrusher(NodeRegistry& registry);   // crusher
void registerGater(NodeRegistry& registry);   // gater
void registerMultiTap(NodeRegistry& registry);   // multitap
void registerFreqShift(NodeRegistry& registry);   // freqshift
void registerPhaser(NodeRegistry& registry);   // phaser
void registerSpecPan(NodeRegistry& registry);   // specpan
void registerBlur(NodeRegistry& registry);   // blur
void registerAddSynth(NodeRegistry& registry);   // addsynth
void registerArp(NodeRegistry& registry);   // arp
void registerTape(NodeRegistry& registry);   // tape
void registerString(NodeRegistry& registry);   // string
void registerWavetable(NodeRegistry& registry);   // wavetable
void registerFmSynth(NodeRegistry& registry);   // fmsynth
void registerPiano(NodeRegistry& registry);   // piano
void registerShimmer(NodeRegistry& registry);   // shimmer
void registerShiva(NodeRegistry& registry);   // shiva
void registerWoodblock(NodeRegistry& registry);   // woodblock
void registerCompressor(NodeRegistry& registry);   // compressor
void registerFlanger(NodeRegistry& registry);   // flanger
void registerFilterPro(NodeRegistry& registry);   // filterpro
void registerOberheim(NodeRegistry& registry);   // oberheim
void registerFourpole(NodeRegistry& registry);   // fourpole
void registerSampler(NodeRegistry& registry);   // sampler
void registerGlsl(NodeRegistry& registry);   // glsl (a dummy: the picture is not drawn)
void registerMix(NodeRegistry& registry);   // mix
void registerDcBlock(NodeRegistry& registry);   // dcblock
void registerDistortion(NodeRegistry& registry);   // distortion

}  // namespace wax
