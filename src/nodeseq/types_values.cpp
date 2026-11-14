// The Node Sequencer's nodes that make and change values: Pseq, Pattern, Prand, Pwhite, chance, math, hold, drunk, latch.
// Ports of the same types in webtracker src/nodeseq/nodes.js and logicnodes.js.
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "core/js_math.h"
#include "nodeseq/seq_node.h"
#include "nodeseq/seq_parse.h"

namespace wax::nodeseq {
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// A list typed into a node, read the first time it is needed.
class TypedList {
public:
  const std::vector<double>& of(const Cx& cx) {
    if (!read_) { items_ = parseList(textOf(cx.data(), "text")); read_ = true; }
    return items_;
  }

private:
  bool read_ = false;
  std::vector<double> items_;
};

// Pseq: steps through a list of numbers or notes, one for each event; a dot is a rest
class PseqNode final : public SeqNode {
public:
  explicit PseqNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    if (port == "reset") { idx_ = 0; loops_ = 0; done_ = false; return; }
    const std::vector<double>& list = list_.of(cx_);
    if (list.empty() || done_) return;
    const double v = list[idx_];
    idx_++;
    if (idx_ >= list.size()) {
      idx_ = 0;
      loops_++;
      if (cx_.p("repeats") > 0 && loops_ >= cx_.p("repeats")) done_ = true;
    }
    if (!std::isnan(v)) cx_.emit("value", v, -1);
  }
  void reset() override { idx_ = 0; loops_ = 0; done_ = false; }

private:
  Cx cx_;
  TypedList list_;
  std::size_t idx_ = 0;
  int loops_ = 0;
  bool done_ = false;
};

// Pattern: a row of cells like a small pattern editor: each event steps one cell. A number is sent on, a dot is a rest
// (nothing is sent; the rest port fires instead).
class PatternNode final : public SeqNode {
public:
  explicit PatternNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    if (port == "reset") { idx_ = 0; return; }
    const std::vector<double>& list = list_.of(cx_);
    if (list.empty()) return;
    const std::size_t last = idx_;
    const double v = list[idx_];
    idx_ = (idx_ + 1) % list.size();
    if (std::isnan(v)) cx_.emit("rest", static_cast<double>(last), -1); else cx_.emit("value", v, -1);
  }
  void reset() override { idx_ = 0; }

private:
  Cx cx_;
  TypedList list_;
  std::size_t idx_ = 0;
};

// Prand: picks any value from a list at random, one for each event
class PrandNode final : public SeqNode {
public:
  explicit PrandNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double, double, double) override {
    const std::vector<double>& list = list_.of(cx_);
    if (list.empty()) return;
    const double v = list[static_cast<std::size_t>(std::floor(cx_.rand() * static_cast<double>(list.size())))];
    if (!std::isnan(v)) cx_.emit("value", v, -1);
  }

private:
  Cx cx_;
  TypedList list_;
};

// Pwhite: a random number between two limits for each event, sent out and held as a control
class PwhiteNode final : public SeqNode {
public:
  explicit PwhiteNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double, double, double) override {
    const double lo = cx_.p("lo"), hi = cx_.p("hi");
    double v = lo + cx_.rand() * (hi - lo);
    if (cx_.p("int") >= 0.5) v = jsRound(v);
    cx_.ctl("out", v);
    cx_.emit("value", v, -1);
  }

private:
  Cx cx_;
};

// chance: lets an event through with a probability
class ChanceNode final : public SeqNode {
public:
  explicit ChanceNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override { cx_.emit(cx_.rand() < cx_.p("p") ? "out" : "else", v, vel); }

private:
  Cx cx_;
};

// math: value times a number, plus a number
class MathNode final : public SeqNode {
public:
  explicit MathNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double vel, double) override { cx_.emit("out", v * cx_.p("mul") + cx_.p("add"), vel); }

private:
  Cx cx_;
};

// hold: turns an event's value into a control (lo to hi becomes 0 to 1)
class HoldNode final : public SeqNode {
public:
  explicit HoldNode(Cx cx) : cx_(cx) {}
  void event(const std::string&, double v, double, double) override {
    const double span = cx_.p("hi") - cx_.p("lo");
    cx_.ctl("out", clamp(span == 0 ? 0 : (v - cx_.p("lo")) / span, 0, 1));
  }

private:
  Cx cx_;
};

// drunk: a random walk: each event moves the number up or down by up to "step", kept between lo and hi; sent out and
// held as a control
class DrunkNode final : public SeqNode {
public:
  explicit DrunkNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    const double lo = std::min(cx_.p("lo"), cx_.p("hi")), hi = std::max(cx_.p("lo"), cx_.p("hi"));
    if (port == "reset" || !x_) x_ = (lo + hi) / 2;
    else x_ = std::min(hi, std::max(lo, *x_ + (cx_.rand() * 2 - 1) * cx_.p("step")));
    const double v = cx_.p("int") >= 0.5 ? jsRound(*x_) : *x_;
    cx_.ctl("out", hi == lo ? 0 : (v - lo) / (hi - lo));
    cx_.emit("value", v, -1);
  }
  void reset() override { x_.reset(); }

private:
  Cx cx_;
  std::optional<double> x_;
};

// latch: remembers the last number that arrived at in, and sends it again each time an event arrives at fire
class LatchNode final : public SeqNode {
public:
  explicit LatchNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "in") { held_ = v; return; }
    if (!std::isnan(held_)) cx_.emit("out", held_, vel);
  }
  void reset() override { held_ = kNaN; }

private:
  Cx cx_;
  double held_ = kNaN;
};

}  // namespace

void addValueTypes(TypeTable& types) {
  types["Pseq"] = makeType<PseqNode>({ev("next"), ev("reset")}, {ev("value")}, {{"repeats", 0}});
  types["Pattern"] = makeType<PatternNode>({ev("next"), ev("reset")}, {ev("value"), ev("rest")}, {{"max", 3}});
  types["Prand"] = makeType<PrandNode>({ev("next")}, {ev("value")}, {});
  types["Pwhite"] = makeType<PwhiteNode>({ev("next")}, {ev("value"), ct("out")}, {{"lo", 0}, {"hi", 1}, {"int", 0}});
  types["chance"] = makeType<ChanceNode>({ev("in")}, {ev("out"), ev("else")}, {{"p", 0.5}});
  types["math"] = makeType<MathNode>({ev("in")}, {ev("out")}, {{"mul", 1}, {"add", 0}});
  types["hold"] = makeType<HoldNode>({ev("in")}, {ct("out")}, {{"lo", 0}, {"hi", 127}});
  types["drunk"] = makeType<DrunkNode>({ev("next"), ev("reset")}, {ev("value"), ct("out")}, {{"lo", 0}, {"hi", 12}, {"step", 2}, {"int", 1}});
  types["latch"] = makeType<LatchNode>({ev("in"), ev("fire")}, {ev("out")}, {});
}

}  // namespace wax::nodeseq
