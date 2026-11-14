// The Node Sequencer's control sources and outputs: lfo, ad, scale, note, set, run, the route dots and monitor.
// Ports of the same types in webtracker src/nodeseq/nodes.js.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "core/js_math.h"
#include "nodeseq/seq_node.h"

namespace wax::nodeseq {
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kPi = 3.141592653589793;

// lfo: a slow wave, as a control
class LfoNode final : public SeqNode {
public:
  explicit LfoNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double, double, double) override { phase_ = 0; }
  bool hasControl() const override { return true; }
  void control(double dt) override {
    const Info& i = cx_.info();
    const bool sync = cx_.p("sync") >= 0.5;
    const double barSec = i.barTicks * i.tickSec;
    const double step = sync ? (barSec > 0 ? dt / barSec * cx_.p("rate") : 0) : dt * cx_.p("rate");
    const double before = phase_;
    phase_ = std::fmod(phase_ + step, 1.0);
    double w;
    switch (jsRoundInt(cx_.p("shape"))) {
      case 1: w = phase_ < 0.5 ? phase_ * 4 - 1 : 3 - phase_ * 4; break;
      case 2: w = phase_ * 2 - 1; break;
      case 3: w = phase_ < 0.5 ? 1 : -1; break;
      case 4: if (phase_ < before) held_ = cx_.rand() * 2 - 1; w = held_; break;   // a new random value each cycle
      default: w = std::sin(phase_ * 2 * kPi);
    }
    cx_.ctl("out", clamp(cx_.p("center") + w * 0.5 * cx_.p("depth"), 0, 1));
  }
  void reset() override { phase_ = 0; }

private:
  Cx cx_;
  double phase_ = 0, held_ = 0.5;
};

// ad: an envelope that rises and falls each time an event arrives, as a control
class AdNode final : public SeqNode {
public:
  explicit AdNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double, double vel, double) override { t_ = 0; top_ = (vel > 0 ? vel : 1) * cx_.p("level"); }
  bool hasControl() const override { return true; }
  void control(double dt) override {
    if (t_ == kInfinity) {
      if (x_ != 0) { x_ = 0; cx_.ctl("out", 0); }
      return;
    }
    t_ += dt * 1000;
    const double a = std::max(0.1, cx_.p("attack"));
    if (t_ < a) {
      x_ = top_ * (t_ / a);
    } else {
      const double d = (t_ - a) / std::max(1.0, cx_.p("decay"));
      x_ = top_ * std::exp(-5 * d);
      if (d > 1.2) { t_ = kInfinity; x_ = 0; }
    }
    cx_.ctl("out", x_);
  }
  void reset() override { t_ = kInfinity; x_ = 0; }

private:
  Cx cx_;
  double t_ = kInfinity, top_ = 1, x_ = 0;   // t: ms since the event, Infinity when the envelope is over
};

// scale: squeezes a control into a smaller range, or turns it upside down
class ScaleNode final : public SeqNode {
public:
  explicit ScaleNode(Cx cx) : cx_(cx) {}
  bool hasControl() const override { return true; }
  void control(double) override {
    const double v = cx_.read("in");
    if (std::isnan(v)) return;
    cx_.ctl("out", cx_.p("lo") + std::pow(clamp(v, 0, 1), cx_.p("curve")) * (cx_.p("hi") - cx_.p("lo")));
  }

private:
  Cx cx_;
};

// note: plays an instrument. mono: a new note ends the one before; poly: notes sound together, each ends on its own.
class NoteNode final : public SeqNode {
public:
  explicit NoteNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double len) override {
    if (port == "pitch") { if (std::isfinite(v)) pitch_ = v; return; }
    const int n = jsRoundInt(port == "note" && std::isfinite(v) ? v : std::isfinite(pitch_) ? pitch_ : cx_.p("pitch"));
    if (n < 0 || n > 127) return;
    const double wiredVel = cx_.read("vel"), velocity = !std::isnan(wiredVel) ? wiredVel : vel > 0 ? vel : cx_.p("vel");
    // a length wired in wins, then the one the event brings, then the knob
    const double wiredLen = cx_.read("len"), lenTicks = !std::isnan(wiredLen) ? 0.05 + wiredLen * 4 : len > 0 ? len : cx_.p("len");
    const double subs = std::max(1.0, jsRound(lenTicks * cx_.info().subs));
    if (cx_.p("poly") >= 0.5) {
      if (held_.count(n)) cx_.noteOff(n);
      cx_.noteOn(n, velocity, -1);
      const int token = ++held_[n];
      cx_.after(subs, [this, n, token] {
        const auto found = held_.find(n);
        if (found != held_.end() && found->second == token) { held_.erase(found); cx_.noteOff(n); }
      }, true);
      return;
    }
    if (sounding_ >= 0) cx_.noteOff(sounding_);
    cx_.noteOn(n, velocity, sounding_);
    sounding_ = n;
    const int mine = ++token_;
    cx_.after(subs, [this, mine] {
      if (token_ == mine && sounding_ >= 0) { cx_.noteOff(sounding_); sounding_ = -1; }
    }, true);
  }
  void reset() override { pitch_ = kNaN; }
  void stop() override {
    if (sounding_ >= 0) { cx_.noteOff(sounding_); sounding_ = -1; }
    for (const auto& entry : held_) cx_.noteOff(entry.first);
    held_.clear();
    token_++;
  }

private:
  Cx cx_;
  double pitch_ = kNaN;
  int sounding_ = -1;        // mono: the note that is on, or -1
  int token_ = 0;            // mono: which note-on a waiting note-off belongs to
  std::map<int, int> held_;  // poly: note -> which note-on of it is the latest
};

// set: moves a parameter of an instrument
class SetNode final : public SeqNode {
public:
  explicit SetNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double, double) override { want_ = clamp(v, 0, 1); }
  bool hasControl() const override { return true; }
  void control(double dt) override {
    const double c = cx_.read("in");
    if (!std::isnan(c)) want_ = clamp(c, 0, 1);
    if (std::isnan(want_)) return;
    const double k = cx_.p("smooth") > 0 ? 1 - std::exp(-dt * 1000 / cx_.p("smooth")) : 1;
    cur_ = std::isnan(cur_) ? want_ : cur_ + (want_ - cur_) * k;
    if (std::abs(cur_ - want_) < 1e-4) cur_ = want_;
    const double x = cx_.p("lo") + cur_ * (cx_.p("hi") - cx_.p("lo"));
    if (std::isnan(sent_) || x != sent_) { sent_ = x; cx_.setParam(x); }
  }
  void reset() override { cur_ = want_ = sent_ = kNaN; }

private:
  Cx cx_;
  double cur_ = kNaN, want_ = kNaN, sent_ = kNaN;
};

// run: starts, stops or flips a group of nodes when an event arrives
class RunNode final : public SeqNode {
public:
  explicit RunNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double, double, double) override { cx_.setGroup(jsRoundInt(cx_.p("group")) - 1, jsRoundInt(cx_.p("mode"))); }

private:
  Cx cx_;
};

// Route points: a dot that passes on whatever comes in, for bending a wire around other nodes. One for each kind of wire.
class RouteEvent final : public SeqNode {
public:
  explicit RouteEvent(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override { cx_.emit("out", v, vel); }

private:
  Cx cx_;
};

class RouteControl final : public SeqNode {
public:
  explicit RouteControl(Cx cx) : cx_(cx) {}
  bool hasControl() const override { return true; }
  void control(double) override { const double v = cx_.read("in"); if (!std::isnan(v)) cx_.ctl("out", v); }

private:
  Cx cx_;
};

class RoutePulled final : public SeqNode {
public:
  explicit RoutePulled(Cx cx) : cx_(cx) {}
  std::optional<Pulled> get(const std::string&) override { return cx_.pull("in"); }

private:
  Cx cx_;
};

}  // namespace

void addOutputTypes(TypeTable& types) {
  types["lfo"] = makeType<LfoNode>({ev("reset")}, {ct("out")}, {{"rate", 0.5}, {"sync", 0}, {"shape", 0}, {"depth", 1}, {"center", 0.5}});
  types["ad"] = makeType<AdNode>({ev("trig")}, {ct("out")}, {{"attack", 5}, {"decay", 250}, {"level", 1}});
  types["scale"] = makeType<ScaleNode>({ct("in")}, {ct("out")}, {{"lo", 0}, {"hi", 1}, {"curve", 1}});
  types["note"] = makeType<NoteNode>({ev("trig"), ev("pitch"), ev("note"), ct("vel"), ct("len")}, {}, {{"pitch", 60}, {"vel", 0.8}, {"len", 1}, {"poly", 0}});
  types["set"] = makeType<SetNode>({ct("in"), ev("set")}, {}, {{"lo", 0}, {"hi", 1}, {"smooth", 30}});
  types["run"] = makeType<RunNode>({ev("in")}, {}, {{"group", 2}, {"mode", 0}});
  types["route"] = makeType<RouteEvent>({ev("in")}, {ev("out")}, {});
  types["routeC"] = makeType<RouteControl>({ct("in")}, {ct("out")}, {});
  types["routeH"] = makeType<RoutePulled>({{"in", PortKind::Chord}}, {{"out", PortKind::Chord}}, {});
  types["routeW"] = makeType<RoutePulled>({{"in", PortKind::Weights}}, {{"out", PortKind::Weights}}, {});
  types["monitor"] = makeType<RouteEvent>({ev("in")}, {ev("out")}, {});   // shows the events that pass through it (on the page); here it only passes them on
}

}  // namespace wax::nodeseq
