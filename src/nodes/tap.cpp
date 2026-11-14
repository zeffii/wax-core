// Tap: records the signal passing through it into a sample slot, started and stopped from a pattern. The audio goes on to
// the output untouched. Port of webtracker src/audio/nodes/tap.js. The node only gathers audio: blocks of it go out through
// NodeContext::emit as TakeMessages, and engine/takes.h assembles the sample.
#include <algorithm>
#include <cstring>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum { kTrig, kLen, kSmp, kCh, kFade, kFadeIn, kCopy };
constexpr int kChunk = 8192;
constexpr double kMaxSeconds = 120;

class Tap final : public Node {
public:
  explicit Tap(const NodeContext& context) : sr_(context.sampleRate), clock_(context.clock), emit_(context.emit) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (clock_) {
      const std::uint32_t stamp = clock_->stamps[static_cast<std::size_t>(slots[kTrig])];
      if (!seen_) { seen_ = true; stamp_ = stamp; }
      else if (stamp != stamp_) {   // the pattern wrote the trig cell
        stamp_ = stamp;
        flush(true);
        const int slot = jsRoundInt(params[slots[kSmp]]);
        if (params[slots[kTrig]] >= 1 && slot >= 1 && slot <= 16) {
          const int ch = jsRoundInt(params[slots[kCh]]) >= 2 ? 2 : 1;
          take_ = true;
          id_ = ++counter_;
          ch_ = ch;
          l_.assign(kChunk, 0.0f);
          r_.assign(ch == 2 ? kChunk : 0, 0.0f);
          fill_ = 0;
          frames_ = 0;
          startTick_ = clock_->tickId;
          ticks_ = jsRoundInt(params[slots[kLen]]);
          TakeMessage m;
          m.phase = TakeMessage::Phase::Start;
          m.take = id_;
          m.smp = slot;
          m.ch = ch;
          m.rate = sr_;
          m.ticks = ticks_;
          m.fade = params[slots[kFade]];
          m.fadeIn = params[slots[kFadeIn]];
          m.copy = jsRoundInt(params[slots[kCopy]]);
          send(m);
        }
      }
    }
    if (take_) {
      // a take ends on the tick boundary, when the song stops, or at the cap
      if ((ticks_ > 0 && clock_->tickId - startTick_ >= ticks_) || !clock_->playing || frames_ >= kMaxSeconds * sr_) flush(true);
    }
    if (take_) {
      for (int i = 0; i < n; i++) {
        const std::size_t f = static_cast<std::size_t>(fill_);
        if (ch_ == 2) { l_[f] = inL[i]; r_[f] = inR[i]; }
        else l_[f] = static_cast<float>((static_cast<double>(inL[i]) + static_cast<double>(inR[i])) * 0.5);
        if (++fill_ == kChunk) { frames_ += kChunk; flush(false); }
      }
    }
    if (outL != inL) std::memmove(outL, inL, static_cast<std::size_t>(n) * sizeof(float));
    if (outR != inR) std::memmove(outR, inR, static_cast<std::size_t>(n) * sizeof(float));
  }

private:
  double sr_;
  const Clock* clock_;
  std::function<void(const TakeMessage&)> emit_;
  bool seen_ = false, take_ = false;
  std::uint32_t stamp_ = 0;
  int counter_ = 0, id_ = 0, ch_ = 2, fill_ = 0, ticks_ = 0, startTick_ = 0;
  double frames_ = 0;
  std::vector<float> l_, r_;

  void send(const TakeMessage& m) { if (emit_) emit_(m); }

  void flush(bool final) {
    if (!take_) return;
    if (fill_) {
      TakeMessage m;
      m.phase = TakeMessage::Phase::Chunk;
      m.take = id_;
      m.l.assign(l_.begin(), l_.begin() + fill_);
      if (ch_ == 2) m.r.assign(r_.begin(), r_.begin() + fill_);
      send(m);
      fill_ = 0;
    }
    if (final) {
      TakeMessage m;
      m.phase = TakeMessage::Phase::End;
      m.take = id_;
      send(m);
      take_ = false;
    }
  }
};

}  // namespace

void registerTap(NodeRegistry& registry) {
  NodeType type;
  type.type = "tap";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"trig", 0, 1, 0, true}, {"len", 0, 255, 16, true}, {"smp", 0, 16, 1, true}, {"ch", 1, 2, 2, true},
                 {"fade", 0, 255, 20, true}, {"fadein", 0, 255, 5, true}, {"copy", 0, 255, 0, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<Tap>(context); };
  registry.add(std::move(type));
}

}  // namespace wax
