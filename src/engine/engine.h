// Everything a song needs to be played, built from a project: the parameter slots, the runtime with every instrument
// loaded (and its samples), and the sequencer with the song, the lanes and the Node Sequencer. No sound card and no
// window: offline.cpp renders it to a file, the live player feeds it to a sound device.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/params.h"
#include "core/spsc.h"
#include "engine/runtime.h"
#include "engine/sequencer.h"
#include "project/project.h"
#include "seq/columns.h"

namespace wax {

inline constexpr int kEngineBlock = 128;   // the engine works in blocks of this many frames

class Engine {
public:
  // Throws std::runtime_error when the project cannot be played (an unknown node type, a pattern that cannot be read).
  Engine(const Project& project, double sampleRate, int threads = 1);
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  GraphRuntime& runtime() { return runtime_; }
  const ParamRegistry& registry() const { return registry_; }
  // The loudest sample an instrument made since the last call (for the mixer's meters).
  float takeLevel(const std::string& instrument) { return runtime_.takeLevel(instrument); }
  // A parameter's value now (the window thread; the audio thread picks it up in its next block). Unknown addresses are ignored.
  void setParam(const std::string& address, double value);
  Sequencer& sequencer() { return sequencer_; }
  const std::vector<std::string>& warnings() const { return warnings_; }          // what the song uses that is not ported yet
  const std::vector<std::string>& instrumentIds() const { return instrumentIds_; }   // in the order of the file
  // Short headings for the cells of a lane's rows ("N1", "V1", "T2", the parameter's name, "CMD"); empty for an unknown lane.
  const std::vector<std::string>& columnLabels(const std::string& laneId) const;
  // The columns of a lane (their kinds say what each cell holds); empty for an unknown lane.
  const std::vector<Column>& laneColumns(const std::string& laneId) const;

  // Changing a lane while the song plays. The window thread builds the lane from the edited project (slow; throws
  // std::runtime_error when a pattern cannot be read, and then nothing changes), and sends it; the audio thread takes
  // what has been sent at the start of its next block and swaps it in. The replaced lanes are collected on the window
  // thread, so nothing is freed while sound is being made.
  bool sendLane(const Project& project, const std::string& laneId);   // window thread; false when the queue is full: try again
  void applySentLanes();                                              // audio thread, at the start of a block
  void collectRetired();                                              // window thread, now and then

  // A node's data (its drawn envelopes) changed while the song plays: sent like a lane, taken at the start of the next block.
  // Compiles an instrument's graph as the project now holds it and sends it to the audio thread (see GraphRuntime::sendGraph). Empty string
  // when sent; otherwise why not (a loop in the wiring, a full queue), and the sound goes on as it was.
  // The song loop in ticks (end <= start: none). Takes effect at the next tick.
  void setLoop(int start, int end) { sequencer_.setLoop(start, end); }
  std::string sendGraph(const Project& project, const std::string& instrument);
  bool sendData(const std::string& instrument, const std::string& nodeId, Json data);   // false when the queue is full: try again later

private:
  ParamRegistry registry_;
  SpscQueue<std::shared_ptr<void>, 64> sent_, retired_;
  GraphRuntime runtime_;
  Sequencer sequencer_;
  std::vector<std::string> warnings_;
  std::vector<std::string> instrumentIds_;
  std::map<std::string, std::vector<std::string>> labels_;
  std::map<std::string, std::vector<Column>> columns_;
};

}  // namespace wax
