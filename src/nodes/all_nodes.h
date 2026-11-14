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
void registerChord(NodeRegistry& registry);
void registerChorus(NodeRegistry& registry);
void registerClap(NodeRegistry& registry);
void registerCombPerc(NodeRegistry& registry);
void registerControlNodes(NodeRegistry& registry);   // random, times
void registerDuette(NodeRegistry& registry);
void registerEnv(NodeRegistry& registry);
void registerGain(NodeRegistry& registry);
void registerHall(NodeRegistry& registry);
void registerHat808(NodeRegistry& registry);
void registerOsc(NodeRegistry& registry);
void registerPlate(NodeRegistry& registry);
void registerPluckGate(NodeRegistry& registry);
void registerSquelch(NodeRegistry& registry);
void registerSubSynth(NodeRegistry& registry);
void registerWaveFolder(NodeRegistry& registry);
void registerWestCoast(NodeRegistry& registry);

}  // namespace wax
