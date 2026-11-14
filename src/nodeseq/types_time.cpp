// The Node Sequencer's nodes that deal in time and rhythm: clock, every, euclid, steps, delay, section, modulo.
// Ports of the same types in webtracker src/nodeseq/nodes.js and logicnodes.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodeseq/seq_node.h"
#include "nodeseq/seq_parse.h"

namespace wax::nodeseq {
namespace {

// clock: the song's tick and the subticks between ticks
class ClockNode final : public SeqNode {
public:
  explicit ClockNode(Cx cx) : cx_(cx) {}
  void tick() override {
    const Info& i = cx_.info();
    cx_.emit("tick", i.tick, -1);
    if (i.tick % i.tpb == 0) cx_.emit("beat", i.tick / i.tpb, -1);
    if (i.tick % i.barTicks == 0) cx_.emit("bar", i.tick / i.barTicks, -1);
  }
  void sub() override { cx_.emit("sub", cx_.info().subIdx, -1); }

private:
  Cx cx_;
};

// every: passes every nth event
class EveryNode final : public SeqNode {
public:
  explicit EveryNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "reset") { count_ = 0; return; }
    const long n = std::max(1L, static_cast<long>(jsRound(cx_.p("n"))));
    if (count_ % n == static_cast<long>(jsRound(cx_.p("offset"))) % n) cx_.emit("out", v, vel);
    count_++;
  }
  void reset() override { count_ = 0; }

private:
  Cx cx_;
  long count_ = 0;
};

// euclid: spreads pulses evenly over steps, one step for each event
class EuclidNode final : public SeqNode {
public:
  explicit EuclidNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    if (port == "reset") { i_ = 0; return; }
    const int s = std::max(1, jsRoundInt(cx_.p("steps")));
    const int last = i_ % s;
    cx_.emit(euclidHit(last, s, cx_.p("pulses"), cx_.p("rot")) ? "hit" : "miss", last, -1);
    i_ = (last + 1) % s;
  }
  void reset() override { i_ = 0; }

private:
  Cx cx_;
  int i_ = 0;
};

// steps: a row of steps. With a list of probabilities, a step holds which one it has and fires that often; with no
// list, a step is on or off with a level.
class StepsNode final : public SeqNode {
public:
  explicit StepsNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    if (port == "reset") { i_ = 0; return; }
    const int n = std::max(1, jsRoundInt(cx_.p("n")));
    const int last = i_ % n;
    const double c = cellAt(last);
    i_ = (last + 1) % n;
    const std::vector<double> probs = parseProbs(textOf(cx_.data(), "probs"));
    if (!probs.empty()) {   // 1 is the first probability of the list, 0 is off; rolled again every pass
      const int k = jsRoundInt(c);
      if (k > 0 && cx_.rand() < probs[static_cast<std::size_t>(std::min(k, static_cast<int>(probs.size())) - 1)]) { cx_.ctl("val", 1); cx_.emit("out", last, 1); }
      return;
    }
    if (c > 0) { cx_.ctl("val", c); cx_.emit("out", last, c); }
  }
  void reset() override { i_ = 0; }

private:
  double cellAt(int index) const {
    const Json& data = cx_.data();
    if (!data.is_object() || !data.contains("cells") || !data["cells"].is_array()) return 0;
    const Json& cells = data["cells"];
    if (index < 0 || static_cast<std::size_t>(index) >= cells.size()) return 0;
    const Json& cell = cells[static_cast<std::size_t>(index)];
    return cell.is_number() ? cell.get<double>() : cell.is_boolean() ? (cell.get<bool>() ? 1 : 0) : 0;
  }

  Cx cx_;
  int i_ = 0;
};

// delay: passes an event on some subticks later
class DelayNode final : public SeqNode {
public:
  explicit DelayNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override {
    const Cx cx = cx_;
    cx_.after(jsRound(cx_.p("subs")), [cx, v, vel] { cx.emit("out", v, vel); });
  }

private:
  Cx cx_;
};

// section: passes the events of the clock on only while the song is between start and end: a part of the song.
// start and end are events too, on the first and the last tick; `on` is a control that is 1 inside; `pos` runs 0 to 1
// across the section. With `from` set to the event's value it watches the number each event carries instead of the
// song's tick (a counter, or the rem of a modulo).
class SectionNode final : public SeqNode {
public:
  explicit SectionNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override {
    if (!byValue()) {
      if (inside(cx_.info().tick)) cx_.emit("out", v, vel);
      return;
    }
    const double at = jsRound(v);   // each event is a position; the section reacts to it as it arrives
    step(at);
    if (inside(at)) cx_.emit("out", v, vel);
  }
  void tick() override { if (!byValue()) step(cx_.info().tick); }
  void reset() override { was_ = false; }

private:
  struct Span {
    double first, last;
  };
  // the first and the last tick of the section (in bars, a bar is barTicks ticks)
  Span span() const {
    const double k = jsRoundInt(cx_.p("unit")) == 1 ? cx_.info().barTicks : 1, end = jsRound(cx_.p("end")) * k;
    return {jsRound(cx_.p("start")) * k, cx_.p("inclusive") >= 0.5 ? end : end - 1};
  }
  bool byValue() const { return cx_.p("from") >= 0.5; }
  bool inside(double tick) const { const Span s = span(); return tick >= s.first && tick <= s.last; }
  void step(double t) {
    const Span s = span();
    const bool now = t >= s.first && t <= s.last;
    if (now && !was_) cx_.emit("start", t, -1);
    if (now && t == s.last) cx_.emit("end", t, -1);
    was_ = now;
    cx_.ctl("on", now ? 1 : 0);
    cx_.ctl("pos", now ? (s.last == s.first ? 0 : (t - s.first) / (s.last - s.first)) : 0);
  }

  Cx cx_;
  bool was_ = false;
};

// modulo: looks at the song's tick, not at how many events have come: an event passes when its tick is on every nth
// tick (or beat, or bar), counting from offset; the others leave by else. hit fires on those ticks, rem sends the tick
// modulo n on every tick, and phase is a control that climbs from 0 to 1 across each period.
class ModuloNode final : public SeqNode {
public:
  explicit ModuloNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override { cx_.emit(place(cx_.info().tick).at == 0 ? "out" : "else", v, vel); }
  void tick() override {
    const Place p = place(cx_.info().tick);
    if (p.at == 0) cx_.emit("hit", static_cast<double>(p.count), -1);
    cx_.emit("rem", static_cast<double>(p.at), -1);
    cx_.ctl("phase", static_cast<double>(p.at) / static_cast<double>(p.period));
  }

private:
  struct Place {
    long period, at, count;
  };
  Place place(long tick) const {
    const int which = jsRoundInt(cx_.p("unit"));
    const long unit = which == 1 ? cx_.info().tpb : which == 2 ? cx_.info().barTicks : 1;
    const long period = std::max(1L, static_cast<long>(jsRound(cx_.p("n")))) * unit, t = tick - static_cast<long>(jsRound(cx_.p("offset"))) * unit;
    return {period, ((t % period) + period) % period, std::max(0L, static_cast<long>(std::floor(static_cast<double>(t) / static_cast<double>(period))))};
  }

  Cx cx_;
};

}  // namespace

void addTimeTypes(TypeTable& types) {
  types["clock"] = makeType<ClockNode>({}, {ev("tick"), ev("sub"), ev("beat"), ev("bar")}, {});
  types["every"] = makeType<EveryNode>({ev("in"), ev("reset")}, {ev("out")}, {{"n", 4}, {"offset", 0}});
  types["euclid"] = makeType<EuclidNode>({ev("in"), ev("reset")}, {ev("hit"), ev("miss")}, {{"steps", 8}, {"pulses", 5}, {"rot", 0}});
  types["steps"] = makeType<StepsNode>({ev("in"), ev("reset")}, {ev("out"), ct("val")}, {{"n", 16}});
  types["delay"] = makeType<DelayNode>({ev("in")}, {ev("out")}, {{"subs", 2}});
  types["section"] = makeType<SectionNode>({ev("in")}, {ev("out"), ev("start"), ev("end"), ct("on"), ct("pos")},
                                           {{"start", 0}, {"end", 64}, {"inclusive", 0}, {"unit", 0}, {"from", 0}});
  types["modulo"] = makeType<ModuloNode>({ev("in")}, {ev("out"), ev("else"), ev("hit"), ev("rem"), ct("phase")}, {{"n", 4}, {"offset", 0}, {"unit", 0}});
}

}  // namespace wax::nodeseq
