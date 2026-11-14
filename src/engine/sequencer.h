// Plays the song: splits each block at tick boundaries so events land on the exact sample, writes parameter columns
// into the parameter block, and sends note events to the lanes' instruments.
// Port of webtracker src/audio/sequencer.js.
//
// Not ported yet: lane curves (automation) and the messages a preset sends to nodes.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/limiter.h"
#include "engine/runtime.h"
#include "nodeseq/nodeseq.h"
#include "seq/lane.h"

namespace wax {

// What the sequencer needs of the song. The tempo in the header is where the song starts; after that the master
// lane's cells move the tempo parameters, which are read every tick.
struct SongSettings {
  double bpm = 120;
  int ticksPerBeat = 4;
  int length = 16;
  double swing = 0.5;
  int beatsPerBar = 4;
  MasterSlots master;   // not present: the song's own tempo is used
};

class Sequencer {
public:
  Sequencer(double sampleRate, GraphRuntime& runtime, int maxBlock = 512);
  ~Sequencer();

  void setSong(const SongSettings& song);
  void setLane(const PreparedLane& lane);
  // The graph of the Node Sequencer (the song's `nodeseq`), with what its names mean on this machine.
  void setNodeseq(const Json& graph, const nodeseq::Resolve& resolve);
  const nodeseq::NodeSeq& nodeseq() const { return nodeseq_; }

  // From a tick. With `once`, the song is played one time: when its last row has had its full length the sequencer
  // stops (every note is released) instead of starting again from the top.
  void play(int from = 0, bool once = false);
  void stop();

  // Fills left and right with n samples (n is at most maxBlock).
  void process(float* left, float* right, int n);

  bool playing() const { return playing_; }
  int tick() const { return tick_; }

private:
  struct CommandTable;
  struct LaneState;
  struct Tempo {
    double bpm;
    double swing;
    int tpb;
  };

  Tempo tempo() const;
  void resetTempo();
  void fireTick();
  void playRow(LaneState& lane);
  void release(LaneState& lane, int noteColumn, bool implied = false);
  void allOff(LaneState& lane);
  void releaseAll();
  void writeParam(int slot, double min, double max, bool log, double x);
  void touch(int slot);

  double sampleRate_;
  GraphRuntime& runtime_;
  float* params_;
  Limiter limiter_;
  // the Node Sequencer runs on the same clock, subdividing each tick (the swing of a tick carries into its subticks)
  nodeseq::NodeSeq nodeseq_;
  int subIdx_ = 0;
  double subLen_ = 0, subCountdown_ = 0;
  std::vector<float> scratchLeft_, scratchRight_;

  SongSettings song_;
  bool songSet_ = false;
  std::vector<std::unique_ptr<LaneState>> lanes_;   // in the order they were first set
  int nextColumnKey_ = 0;

  int tick_ = 0;
  double songPos_ = 0;     // where the song is, in ticks and fractions of one
  double countdown_ = 0;   // samples until the next tick
  double tickLen_ = 0;     // samples in the tick now sounding (swing makes them differ)
  bool playing_ = false;
  bool once_ = false;
  bool wrapped_ = false;   // once: the last row has fired
  bool gainSet_ = false;
  double gain_ = 1;        // the master volume the last span ended on
};

}  // namespace wax
