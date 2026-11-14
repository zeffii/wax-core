// The Node Sequencer's nodes that decide where events go and when: switch, pick, gate, counter, compare, flip, once, ratchet.
// Ports of the same types in webtracker src/nodeseq/logicnodes.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodeseq/seq_node.h"

namespace wax::nodeseq {
namespace {

long rounded(double v) { return static_cast<long>(jsRound(v)); }

// switch: sends each event out of one of four outputs. A number at pick chooses which (0 is the first); or, with "by"
// set to value, the event's own number chooses.
class SwitchNode final : public SeqNode {
public:
  explicit SwitchNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "pick") { at_ = std::max(0L, rounded(v)) % 4; return; }
    const long which = cx_.p("by") >= 0.5 ? ((rounded(v) % 4) + 4) % 4 : at_;
    cx_.emit("o" + std::to_string(which + 1), v, vel);
  }
  void reset() override { at_ = 0; }

private:
  Cx cx_;
  long at_ = 0;
};

// pick: lets only one of four inputs through: a number at pick chooses which (0 is the first)
class PickNode final : public SeqNode {
public:
  explicit PickNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "pick") { at_ = std::max(0L, rounded(v)) % 4; return; }
    if (port.size() == 1 && port[0] - 'a' == at_) cx_.emit("out", v, vel);
  }
  void reset() override { at_ = 0; }

private:
  Cx cx_;
  long at_ = 0;
};

// gate: combines two event streams. and: an event at a or b goes through only if the other has fired in the last
// "window" subticks; or: either goes through; a unless b: a goes through unless b fired in the window.
class GateNode final : public SeqNode {
public:
  explicit GateNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    const double now = static_cast<double>(cx_.info().abs), window = jsRound(cx_.p("window"));
    const int mode = jsRoundInt(cx_.p("mode"));
    if (port == "a") lastA_ = now; else lastB_ = now;
    const double other = port == "a" ? lastB_ : lastA_;
    const bool near = now - other <= window;
    if (mode == 0 ? near : mode == 1 ? true : (port == "a" && !near)) cx_.emit("out", v, vel);
  }
  void reset() override { lastA_ = lastB_ = -1e9; }

private:
  Cx cx_;
  double lastA_ = -1e9, lastB_ = -1e9;
};

// counter: counts the events that arrive and sends the count (0, 1, 2 ...), wrapping round at n; wrap sends an event
// each time it wraps
class CounterNode final : public SeqNode {
public:
  explicit CounterNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    if (port == "reset") { count_ = 0; return; }
    const long n = std::max(1L, rounded(cx_.p("n"))), last = count_ % n;
    cx_.emit("out", static_cast<double>(last), -1);
    count_ = last + 1;
    if (count_ >= n) { count_ = 0; cx_.emit("wrap", static_cast<double>(last), -1); }
  }
  void reset() override { count_ = 0; }

private:
  Cx cx_;
  long count_ = 0;
};

// compare: tests an event's number against x: the event leaves by yes if the test is true, by no if not
class CompareNode final : public SeqNode {
public:
  explicit CompareNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override {
    const double x = cx_.p("x");
    const int t = jsRoundInt(cx_.p("test"));
    const bool yes = t == 0 ? v > x : t == 1 ? v < x : t == 2 ? v == x : t == 3 ? v >= x : t == 4 ? v <= x : v != x;
    cx_.emit(yes ? "yes" : "no", v, vel);
  }

private:
  Cx cx_;
};

// flip: sends events out of a, then b, then a again
class FlipNode final : public SeqNode {
public:
  explicit FlipNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "reset") { on_ = false; return; }
    cx_.emit(on_ ? "b" : "a", v, vel);
    on_ = !on_;
  }
  void reset() override { on_ = false; }

private:
  Cx cx_;
  bool on_ = false;
};

// once: lets through only the nth event after the start (or after reset), and nothing else
class OnceNode final : public SeqNode {
public:
  explicit OnceNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "reset") { seen_ = 0; return; }
    seen_++;
    if (seen_ == rounded(cx_.p("n"))) cx_.emit("out", v, vel);
  }
  void reset() override { seen_ = 0; }

private:
  Cx cx_;
  long seen_ = 0;
};

// ratchet: repeats an event: it is sent "count" times, "gap" subticks apart, each a little quieter by "fade"
class RatchetNode final : public SeqNode {
public:
  explicit RatchetNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override {
    const long n = std::max(1L, rounded(cx_.p("count"))), gap = std::max(1L, rounded(cx_.p("gap")));
    const double base = vel >= 0 ? vel : 1, fade = cx_.p("fade");
    const Cx cx = cx_;
    for (long k = 0; k < n; k++) {
      const double level = vel >= 0 || fade > 0 ? base * (1 - fade * static_cast<double>(k) / static_cast<double>(n)) : -1;
      if (k == 0) cx_.emit("out", v, level);
      else cx_.after(static_cast<double>(k * gap), [cx, v, level] { cx.emit("out", v, level); });
    }
  }

private:
  Cx cx_;
};

}  // namespace

void addLogicTypes(TypeTable& types) {
  types["switch"] = makeType<SwitchNode>({ev("in"), ev("pick")}, {ev("o1"), ev("o2"), ev("o3"), ev("o4")}, {{"by", 0}});
  types["pick"] = makeType<PickNode>({ev("pick"), ev("a"), ev("b"), ev("c"), ev("d")}, {ev("out")}, {});
  types["gate"] = makeType<GateNode>({ev("a"), ev("b")}, {ev("out")}, {{"mode", 0}, {"window", 0}});
  types["counter"] = makeType<CounterNode>({ev("in"), ev("reset")}, {ev("out"), ev("wrap")}, {{"n", 8}});
  types["compare"] = makeType<CompareNode>({ev("in")}, {ev("yes"), ev("no")}, {{"test", 0}, {"x", 0}});
  types["flip"] = makeType<FlipNode>({ev("in"), ev("reset")}, {ev("a"), ev("b")}, {});
  types["once"] = makeType<OnceNode>({ev("in"), ev("reset")}, {ev("out")}, {{"n", 1}});
  types["ratchet"] = makeType<RatchetNode>({ev("in")}, {ev("out")}, {{"count", 3}, {"gap", 1}, {"fade", 0}});
}

}  // namespace wax::nodeseq
