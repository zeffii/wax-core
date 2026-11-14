// Runs compiled graphs block by block: every instrument's nodes in order, its voices, its sleep, and the sum of all
// instruments into the mix. It knows nothing about patterns (the sequencer drives it) or about sound cards.
// Port of webtracker src/audio/runtime.js.
//
// Not ported yet (each is named where it would go): the carve stage of the mixer, node messages (samples, drawn curves),
// and the things only the page uses (scopes, the Stream Manager's switches and meters).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/node.h"
#include "core/node_registry.h"
#include "engine/sleep.h"
#include "graph/compile.h"

namespace wax {

inline constexpr int kMaxVoices = 16;

// The parameter slots of the master lane's tempo node, which the sequencer and the mixer read. -1: not there.
struct MasterSlots {
  int bpm = -1, swing = -1, tpb = -1, volume = -1;
  int limit = -1, ceiling = -1, boost = -1, release = -1;
  int carve = -1, cvamt = -1, cvdepth = -1, cvfocus = -1;

  bool present() const { return bpm >= 0; }
};

class GraphRuntime {
public:
  GraphRuntime(double sampleRate, float* params, const NodeRegistry& nodeTypes, int maxBlock = 512);
  ~GraphRuntime();

  // Allocates buffers and node state for an instrument (again, when it is already loaded). Never called per block.
  void load(const CompiledGraph& compiled);

  // A note for an instrument: to its arp when it has one, else to its voices (see deliver).
  void note(const std::string& instrument, const NoteEvent& event);

  // Kill (the ^^^ cell): the instrument fades out quickly, is reset, and is not run until its next note-on.
  void kill(const std::string& instrument);

  // Which machines each meta machine plays. Replaces what was set before.
  void setMetaSources(const std::map<std::string, std::vector<std::string>>& map);

  void setDelayComp(bool on);

  // How many samples everything is held back by, with delay compensation on: the delay of the slowest instrument.
  int alignment();

  // Samples the whole mix is late by because of the carve stage. (The carve stage is not ported yet: always 0.)
  int carveLatency() const { return 0; }

  // Fills left and right with n samples of the mix (n is at most maxBlock).
  void process(float* left, float* right, int n);

  Clock& clock() { return clock_; }
  double sampleRate() const { return sampleRate_; }
  float* params() { return params_; }

  MasterSlots masterSlots;   // set by the sequencer when it is given the song

  // True once a song has switched on something this engine does not do yet (the render then differs from the web app's).
  bool sawUnported() const { return sawUnported_; }

private:
  struct RunNode;
  struct VoiceSet;
  struct Instrument;
  struct Listener;

  Instrument* find(const std::string& id);
  std::unique_ptr<RunNode> makeNode(const CompiledNode& compiled, const std::string& instrument, std::vector<std::vector<float>>& own, std::vector<std::vector<float>>& shared, const std::set<int>& sharedBuffers);
  void runNode(Instrument& inst, RunNode& node, int n);
  void deliver(const std::string& instrument, const NoteEvent& event);
  void passOn(const std::string& instrument, const NoteEvent& event);
  std::vector<Listener>* listenersOf(const std::string& instrument);
  const std::vector<std::string>& runOrder();
  void rebuild(const std::string& id);
  bool dozing(const std::string& id, Instrument& inst, int n);
  void settle(Instrument& inst, int n);

  double sampleRate_;
  float* params_;
  const NodeRegistry& nodeTypes_;
  int maxBlock_;
  Clock clock_;
  Bus bus_;

  std::vector<std::string> loadOrder_;                                        // instrument ids, in the order they were first loaded
  std::unordered_map<std::string, std::unique_ptr<Instrument>> instruments_;
  std::unordered_map<std::string, std::unique_ptr<CompiledGraph>> graphs_;    // kept to rebuild an instrument that went bad

  bool sleepIdle_ = true;
  bool delayComp_ = false;
  std::optional<int> maxDelay_;
  std::uint64_t stamp_ = 0;            // counts note events, to find the oldest voice
  std::set<std::string> killed_;       // faded out and reset: not run until a note-on
  std::set<std::string> dying_;        // their kill fade has just finished: reset at the start of the next block
  std::set<std::string> following_;    // instruments whose notes are being passed on right now (a loop of listeners stops where it closes)
  std::map<std::string, std::vector<std::string>> metaTargets_;
  std::optional<std::vector<std::string>> order_;
  std::optional<std::map<std::string, std::vector<Listener>>> followMap_;
  std::map<std::string, int> instancesMade_;   // per kind of dice-rolling node (see NodeContext::nextInstance)
  std::int64_t blockNo_ = 0;
  bool sawUnported_ = false;
};

}  // namespace wax
