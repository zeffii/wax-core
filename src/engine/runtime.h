// Runs compiled graphs block by block: every instrument's nodes in order, its voices, its sleep, and the sum of all
// instruments into the mix. It knows nothing about patterns (the sequencer drives it) or about sound cards.
// Port of webtracker src/audio/runtime.js.
//
// Not ported yet (each is named where it would go): the carve stage of the mixer, node messages other than samples (drawn curves, tables),
// and the things only the page uses (scopes, the Stream Manager's switches and meters).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/node.h"
#include "core/node_registry.h"
#include "core/random.h"
#include "engine/sleep.h"
#include "engine/task_pool.h"
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

  // A sample for one node (every copy of it): the sampler's. Kept, to be sent again when the instrument is rebuilt.
  void sendSample(const std::string& instrument, const std::string& nodeId, std::shared_ptr<const SampleMessage> message);

  // Kill (the ^^^ cell): the instrument fades out quickly, is reset, and is not run until its next note-on.
  void kill(const std::string& instrument);

  // Which machines each meta machine plays. Replaces what was set before.
  void setMetaSources(const std::map<std::string, std::vector<std::string>>& map);

  void setDelayComp(bool on);

  // How many threads render the instruments of a block (the calling thread counts). 1: all on this one. The sound is the
  // same whatever the number: an instrument waits for the ones it hears through a sidechain, and the mix is added up in order.
  void setThreads(int threads);
  int threads() const { return pool_ ? pool_->threads() : 1; }

  // How many samples everything is held back by, with delay compensation on: the delay of the slowest instrument.
  int alignment();

  // Samples the whole mix is late by because of the carve stage. (The carve stage is not ported yet: always 0.)
  int carveLatency() const { return 0; }

  // Fills left and right with n samples of the mix (n is at most maxBlock).
  void process(float* left, float* right, int n);

  Clock& clock() { return clock_; }
  double sampleRate() const { return sampleRate_; }
  float* params() { return params_; }

  // The engine's dice: one stream for everything that calls for a random number where the JS calls Math.random.
  Xorshift32& dice() { return dice_; }

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
  std::optional<StereoView> tap(const std::string& caller, const std::string& id, const Json& parts);
  std::optional<StereoView> tapMix(const std::string& caller, const std::string& id, const std::vector<std::string>& list);
  bool renderInstrument(const std::string& id, int n);
  bool hears(const std::string& caller, const std::string& other) const;
  void resetOrder();
  void rebuild(const std::string& id);
  bool dozing(const std::string& id, Instrument& inst, int n);
  void settle(Instrument& inst, int n);

  double sampleRate_;
  float* params_;
  const NodeRegistry& nodeTypes_;
  int maxBlock_;
  Clock clock_;
  Xorshift32 dice_;
  Bus bus_;

  std::vector<std::string> loadOrder_;                                        // instrument ids, in the order they were first loaded
  std::unordered_map<std::string, std::unique_ptr<Instrument>> instruments_;
  std::unordered_map<std::string, std::unique_ptr<CompiledGraph>> graphs_;    // kept to rebuild an instrument that went bad

  bool sleepIdle_ = true;
  bool delayComp_ = false;
  std::optional<int> maxDelay_;
  std::atomic<std::uint64_t> stamp_{0};   // counts note events, to find the oldest voice (an arp plays notes while its instrument renders)
  std::set<std::string> killed_;       // faded out and reset: not run until a note-on
  std::set<std::string> dying_;        // their kill fade has just finished: reset at the start of the next block
  std::set<std::string> following_;    // instruments whose notes are being passed on right now (a loop of listeners stops where it closes)
  std::map<std::string, std::vector<std::string>> metaTargets_;
  std::optional<std::vector<std::string>> order_;
  struct Plan;
  std::unique_ptr<Plan> plan_;                                   // the order made into tasks for the pool (built with order_)
  std::map<std::string, std::set<std::string>> deps_;             // for each instrument, the ones it hears that run before it (built with order_)
  std::unique_ptr<TaskPool> pool_;
  std::mutex mixMutex_;                                           // the added-together sound of a meta machine is made once per block, by whoever asks first
  std::vector<char> mixesIn_;                                     // per instrument of the order: whether its sound goes into the mix this block
  std::optional<std::map<std::string, std::vector<Listener>>> followMap_;
  std::map<std::string, std::shared_ptr<const SampleMessage>> samples_;   // the last sample sent to each node and slot, replayed on a rebuild
  std::map<std::string, std::shared_ptr<void>> sharedStates_;   // by instrument and node id (see NodeContext::shared)
  std::map<std::string, int> instancesMade_;   // per kind of dice-rolling node (see NodeContext::nextInstance)
  struct Mix;
  std::map<std::string, std::unique_ptr<Mix>> mixes_;   // the added-together sound of a meta machine's machines, one for each way a sidechain asks for it
  std::int64_t blockNo_ = 0;
  int blockSize_ = 0;
  bool sawUnported_ = false;
};

}  // namespace wax
