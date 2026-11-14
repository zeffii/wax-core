// The live side of the engine: the commands a player is given from other threads (the terminal, a window, a MIDI
// keyboard), and the Player, which an audio callback (or a test, by hand) asks for frames. No window, no device here.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "engine/engine.h"
#include "rtmidi/RtMidi.h"

namespace wax {

// Things the player is told, from other threads, and does at the start of the next block.
struct LiveCommand {
  enum Kind : int { None, Play, Stop, NoteOn, NoteOff, AllOff } kind = None;
  int note = 0;
  double vel = 1;
};

// Many producers (the MIDI thread, the terminal) and one consumer (the audio callback). The callback never waits:
// only the producers take the little spin lock.
class LiveRing {
public:
  bool push(const LiveCommand& c) {
    while (lock_.test_and_set(std::memory_order_acquire)) {}
    const unsigned w = write_.load(std::memory_order_relaxed);
    const bool room = w - read_.load(std::memory_order_acquire) < kSize;
    if (room) {
      items_[w % kSize] = c;
      write_.store(w + 1, std::memory_order_release);
    }
    lock_.clear(std::memory_order_release);
    return room;
  }
  bool pop(LiveCommand& c) {
    const unsigned r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    c = items_[r % kSize];
    read_.store(r + 1, std::memory_order_release);
    return true;
  }
private:
  static constexpr unsigned kSize = 1024;
  LiveCommand items_[kSize];
  std::atomic<unsigned> write_{0}, read_{0};
  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

// The engine, its commands, and the carry between the engine's block size and whatever size the sound card asks for.
class Player {
public:
  Player(const Project& project, double rate, int threads, std::string instrument, bool loop)
      : engine_(project, rate, threads), instrument_(std::move(instrument)), once_(!loop), rate_(rate) {}

  LiveRing& ring() { return ring_; }
  Engine& engine() { return engine_; }
  void setInstrument(const std::string& id) { instrument_ = id; }   // from the terminal or the window, between notes
  double peak() const { return peak_.load(); }
  // For a window: what the audio thread last saw (read from any thread).
  int tick() const { return tick_.load(std::memory_order_relaxed); }
  bool playing() const { return playing_.load(std::memory_order_relaxed); }
  float levelLeft() { return levelL_.exchange(0, std::memory_order_relaxed); }    // peak since the last call
  float levelRight() { return levelR_.exchange(0, std::memory_order_relaxed); }

  // Fills n frames of interleaved stereo. Runs on the audio thread.
  void fill(float* out, int frames) {
    int done = 0;
    while (done < frames) {
      if (pos_ >= kEngineBlock) { renderBlock(); pos_ = 0; }
      const int take = std::min(frames - done, kEngineBlock - pos_);
      for (int i = 0; i < take; i++) {
        out[(done + i) * 2] = left_[pos_ + i];
        out[(done + i) * 2 + 1] = right_[pos_ + i];
      }
      pos_ += take;
      done += take;
    }
  }

private:
  void renderBlock() {
    engine_.applySentLanes();   // lanes edited in the window start to play from this block
    LiveCommand c;
    while (ring_.pop(c)) {
      switch (c.kind) {
        case LiveCommand::Play: engine_.sequencer().play(c.note, once_); break;
        case LiveCommand::Stop: engine_.sequencer().stop(); engine_.runtime().note(instrument_, NoteEvent{NoteType::AllOff}); break;
        case LiveCommand::NoteOn: {
          NoteEvent e; e.type = NoteType::On; e.note = c.note; e.vel = c.vel;
          engine_.runtime().note(instrument_, e);
          break;
        }
        case LiveCommand::NoteOff: {
          NoteEvent e; e.type = NoteType::Off; e.note = c.note;
          engine_.runtime().note(instrument_, e);
          break;
        }
        case LiveCommand::AllOff: engine_.runtime().note(instrument_, NoteEvent{NoteType::AllOff}); break;
        case LiveCommand::None: break;
      }
    }
    engine_.sequencer().process(left_, right_, kEngineBlock);
    float p = peak_.load(std::memory_order_relaxed), pl = 0, pr = 0;
    for (int i = 0; i < kEngineBlock; i++) { pl = std::max(pl, std::fabs(left_[i])); pr = std::max(pr, std::fabs(right_[i])); }
    p = std::max(p, std::max(pl, pr));
    peak_.store(p, std::memory_order_relaxed);
    if (pl > levelL_.load(std::memory_order_relaxed)) levelL_.store(pl, std::memory_order_relaxed);
    if (pr > levelR_.load(std::memory_order_relaxed)) levelR_.store(pr, std::memory_order_relaxed);
    tick_.store(engine_.sequencer().tick(), std::memory_order_relaxed);
    playing_.store(engine_.sequencer().playing(), std::memory_order_relaxed);
  }

  Engine engine_;
  LiveRing ring_;
  std::string instrument_;
  bool once_;
  double rate_;
  float left_[kEngineBlock] = {}, right_[kEngineBlock] = {};
  int pos_ = kEngineBlock;   // the whole block is used: the next frame needs a new one
  std::atomic<float> peak_{0}, levelL_{0}, levelR_{0};
  std::atomic<int> tick_{0};
  std::atomic<bool> playing_{false};
};


// Bridges RtMidi to a Player: note on, note off, all notes off (CC 120 and 123).
inline void midiCallback(double, std::vector<unsigned char>* message, void* user) {
  if (message->empty()) return;
  LiveRing& ring = static_cast<Player*>(user)->ring();
  const unsigned char status = (*message)[0] & 0xF0;
  LiveCommand c;
  if (message->size() >= 3 && status == 0x90 && (*message)[2] > 0) {
    c.kind = LiveCommand::NoteOn; c.note = (*message)[1]; c.vel = (*message)[2] / 127.0;
  } else if (message->size() >= 3 && (status == 0x80 || status == 0x90)) {
    c.kind = LiveCommand::NoteOff; c.note = (*message)[1];
  } else if (message->size() >= 3 && status == 0xB0 && ((*message)[1] == 123 || (*message)[1] == 120)) {
    c.kind = LiveCommand::AllOff;
  } else {
    return;
  }
  ring.push(c);
}


}  // namespace wax
