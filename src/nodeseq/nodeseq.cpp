#include "nodeseq/nodeseq.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "core/js_math.h"

namespace wax::nodeseq {
namespace {

constexpr int kFirstColumnKey = 1000000;   // above the keys the pattern lanes hand out

// "node.port" -> { node, port }
std::pair<std::string, std::string> splitEnd(const std::string& text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string::npos) return {text, ""};
  const std::size_t next = text.find('.', dot + 1);
  return {text.substr(0, dot), text.substr(dot + 1, next == std::string::npos ? std::string::npos : next - dot - 1)};
}

const Port* findOut(const SeqNodeType& type, const std::string& name) {
  for (const Port& p : type.outs) if (p.name == name) return &p;
  return nullptr;
}

// One input port of a type by name (a Phrase node's chord ports count as long as the name fits: c1 to c16).
std::optional<Port> findIn(const SeqNodeType& type, const std::string& name) {
  for (const Port& p : type.ins) if (p.name == name) return p;
  const std::string& prefix = type.dynInPrefix;
  if (prefix.empty() || name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
  const std::string digits = name.substr(prefix.size());
  if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) || digits.size() > 3) return std::nullopt;
  const int number = std::stoi(digits);
  return number >= 1 && number <= 16 ? std::optional<Port>(Port{name, type.dynInKind}) : std::nullopt;
}

int groupOf(const Json& node) {
  const Json group = node.value("group", Json(0));
  return std::max(0, std::min(kMaxGroups - 1, group.is_number() ? jsRoundInt(group.get<double>()) : 0));
}

}  // namespace

// ---- what a node sees (Cx)

const std::string& Cx::id() const { return rec_.id; }

double Cx::p(const std::string& name) const {
  const auto found = rec_.p.find(name);
  return found == rec_.p.end() ? std::numeric_limits<double>::quiet_NaN() : found->second;
}

const Json& Cx::data() const { return rec_.data; }
const Info& Cx::info() const { return engine_.info_; }
void Cx::emit(const std::string& out, double value, double vel, double len) const { engine_.emit(rec_, out, value, vel, len); }
bool Cx::has(const std::string& in) const { return rec_.source(in) != nullptr; }

std::optional<Pulled> Cx::pull(const std::string& in) const {
  const Rec::Source* s = rec_.source(in);
  return s ? s->from->inst->get(s->out) : std::nullopt;
}

double Cx::read(const std::string& in) const {
  const Rec::Source* s = rec_.source(in);
  if (!s) return std::numeric_limits<double>::quiet_NaN();
  const auto value = s->from->cout.find(s->out);
  return value == s->from->cout.end() ? 0 : value->second;
}

void Cx::ctl(const std::string& out, double x) const { rec_.cout[out] = x; }
void Cx::after(double subs, std::function<void()> fn, bool always) const { engine_.after(rec_, subs, std::move(fn), always); }
double Cx::rand() const { return engine_.io_.dice->next(); }

namespace {
std::string targetOf(const Json& data) { return data.is_object() && data.contains("target") && data["target"].is_string() ? data["target"].get<std::string>() : ""; }
}  // namespace

void Cx::noteOn(int note, double vel, int from) const {
  const std::string target = targetOf(rec_.data);
  if (target.empty()) return;
  NoteEvent on;
  on.note = note;
  on.vel = vel;
  on.from = from;
  on.column = rec_.columnKey;
  engine_.io_.note(target, on);
}

void Cx::noteOff(int note) const {
  const std::string target = targetOf(rec_.data);
  if (target.empty()) return;
  NoteEvent off;
  off.type = NoteType::Off;
  off.note = note;
  engine_.io_.note(target, off);
}

void Cx::setParam(double x) const {
  const auto found = engine_.resolve_.slots.find(rec_.id);
  if (found == engine_.resolve_.slots.end() || found->second.slot < 0) return;
  const SetTarget& r = found->second;
  double v = r.log ? r.min * js::pow(r.max / r.min, x) : r.min + x * (r.max - r.min);
  if (r.isInt) v = jsRound(v);
  if (std::isfinite(v)) engine_.io_.params[r.slot] = static_cast<float>(v);   // not counted as a pattern write: it is not an event
}

void Cx::setGroup(int group, int mode) const {
  if (group < 0 || group >= kMaxGroups) return;
  engine_.setGroupOn(group, mode == 0 ? true : mode == 1 ? false : !engine_.on_[static_cast<std::size_t>(group)]);
}

const Rec::Source* Rec::source(const std::string& in) const {
  for (const auto& entry : srcs) if (entry.first == in) return &entry.second;
  return nullptr;
}

// ---- the engine

NodeSeq::NodeSeq(double sampleRate, Io io) : sampleRate_(sampleRate), io_(std::move(io)) { on_.fill(true); }

NodeSeq::~NodeSeq() = default;

void NodeSeq::load(const Json& graph, const Resolve& resolve) {
  graph_ = graph;
  resolve_ = resolve;
  const Json sub = graph.value("sub", Json(4));
  const int asked = sub.is_number() ? jsRoundInt(sub.get<double>()) : 4;
  subs_ = std::max(1, std::min(kMaxSubs, asked ? asked : 4));
  info_.subs = subs_;
  list_.clear();
  unknown_.clear();
  queue_.clear();

  std::map<std::string, Rec*> byId;
  for (const Json& n : graph.value("nodes", Json::array())) {
    if (!n.is_object() || !n.contains("id") || !n["id"].is_string()) continue;
    const std::string id = n["id"], type = n.value("type", std::string());
    const SeqNodeType* T = findSeqType(type);
    if (!T) {
      if (std::find(unknown_.begin(), unknown_.end(), type) == unknown_.end()) unknown_.push_back(type);
      continue;
    }
    if (byId.count(id)) continue;
    auto rec = std::make_unique<Rec>();
    rec->id = id;
    rec->type = type;
    rec->p = T->defaults;
    const Json params = n.value("params", Json::object());
    for (const auto& [name, value] : params.items()) if (value.is_number()) rec->p[name] = value.get<double>();
    rec->data = n.value("data", Json::object());
    rec->group = groupOf(n);
    rec->columnKey = kFirstColumnKey + static_cast<int>(list_.size());
    rec->inst = T->create(Cx(*this, *rec));
    byId[id] = rec.get();
    list_.push_back(std::move(rec));
  }

  for (const Json& w : graph.value("wires", Json::array())) {
    if (w.value("off", false)) continue;   // a wire that is switched off carries nothing
    const auto [fromId, fromPort] = splitEnd(w.value("from", std::string()));
    const auto [toId, toPort] = splitEnd(w.value("to", std::string()));
    if (!byId.count(fromId) || !byId.count(toId)) continue;
    Rec* a = byId[fromId];
    Rec* b = byId[toId];
    const Port* out = findOut(*findSeqType(a->type), fromPort);
    const std::optional<Port> in = findIn(*findSeqType(b->type), toPort);
    if (!out || !in || out->kind != in->kind || a == b) continue;
    if (out->kind == PortKind::Event) {
      a->outs[fromPort].push_back({b, toPort});
    } else {   // a control or pulled input has one source: the last wire wins, in the place of the first
      bool replaced = false;
      for (auto& entry : b->srcs) if (entry.first == toPort) { entry.second = {a, fromPort}; replaced = true; }
      if (!replaced) b->srcs.push_back({toPort, {a, fromPort}});
    }
  }
  controlOrder_ = controlOrder();
  if (!live_) reset();
}

// Control wires carry no events: each node computes from the nodes feeding it, so they run sources first.
std::vector<Rec*> NodeSeq::controlOrder() {
  std::vector<Rec*> order;
  std::set<Rec*> seen;
  const std::function<void(Rec*)> visit = [&](Rec* rec) {
    if (!seen.insert(rec).second) return;
    for (const auto& entry : rec->srcs) visit(entry.second.from);
    order.push_back(rec);
  };
  for (const auto& rec : list_) visit(rec.get());
  std::vector<Rec*> withControl;
  for (Rec* rec : order) if (rec->inst->hasControl()) withControl.push_back(rec);
  return withControl;
}

// ---- events

void NodeSeq::emit(Rec& rec, const std::string& out, double value, double vel, double len) {
  const auto wires = rec.outs.find(out);
  if (wires == rec.outs.end() || !on_[static_cast<std::size_t>(rec.group)]) return;
  for (const Rec::Wire& w : wires->second) deliver(*w.to, w.port, value, vel, len);
}

void NodeSeq::deliver(Rec& rec, const std::string& port, double value, double vel, double len) {
  if (depth_ > 32 || !on_[static_cast<std::size_t>(rec.group)]) return;   // a loop of wires stops itself after a while
  depth_++;
  rec.inst->event(port, value, vel, len);
  depth_--;
}

void NodeSeq::after(Rec& rec, double subs, std::function<void()> fn, bool always) {
  const long at = abs_ + std::max(0L, static_cast<long>(jsRound(subs)));
  if (at <= abs_) {
    if (always || on_[static_cast<std::size_t>(rec.group)]) fn();
    return;
  }
  std::size_t i = queue_.size();
  while (i > 0 && queue_[i - 1].at > at) i--;
  queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(i), Queued{at, std::move(fn), &rec, always});
}

void NodeSeq::runDue() {
  while (!queue_.empty() && queue_.front().at <= abs_) {
    Queued item = std::move(queue_.front());
    queue_.erase(queue_.begin());
    if (item.always || on_[static_cast<std::size_t>(item.rec->group)]) item.fn();
  }
}

// ---- groups

void NodeSeq::setGroupOn(int group, bool on) {
  const std::size_t g = static_cast<std::size_t>(group);
  if (on_[g] == on) return;
  on_[g] = on;
  for (const auto& rec : list_) {
    if (rec->group != group) continue;
    if (on) rec->inst->reset(); else rec->inst->stop();
  }
}

// A group a pattern has written to follows the cell: 0 stops it, anything above half starts it.
void NodeSeq::pollGroups() {
  if (!io_.clock) return;
  for (int g = 0; g < kMaxGroups; g++) {
    const int slot = resolve_.groupSlots[static_cast<std::size_t>(g)];
    if (slot < 0) continue;
    const std::uint32_t stamp = io_.clock->stamps[static_cast<std::size_t>(slot)];
    if (stamp != seen_[static_cast<std::size_t>(g)]) {
      seen_[static_cast<std::size_t>(g)] = stamp;
      setGroupOn(g, io_.params[slot] >= 0.5f);
    }
  }
}

// ---- the clock

void NodeSeq::reset() {
  abs_ = -1;
  queue_.clear();
  depth_ = 0;
  const Json groups = graph_.is_object() ? graph_.value("groups", Json::array()) : Json::array();
  for (std::size_t g = 0; g < kMaxGroups; g++) {
    on_[g] = g < groups.size() && groups[g].is_object() ? groups[g].value("on", true) : true;
    const int slot = resolve_.groupSlots[g];
    seen_[g] = slot >= 0 && io_.clock ? io_.clock->stamps[static_cast<std::size_t>(slot)] : 0;
  }
  for (const auto& rec : list_) {
    rec->cout.clear();
    rec->inst->reset();
  }
}

void NodeSeq::start() {
  live_ = true;
  reset();
}

void NodeSeq::stop() {
  live_ = false;
  for (const auto& rec : list_) rec->inst->stop();
  std::vector<Queued> waiting = std::move(queue_);
  queue_.clear();
  for (Queued& item : waiting) if (item.always) item.fn();   // what must happen whatever stops (a note's end)
  queue_.clear();
}

void NodeSeq::onTick(int tick, int tpb, int barTicks, double tickSamples, double bpm) {
  info_.tick = tick;
  info_.subIdx = 0;
  info_.tpb = tpb;
  info_.barTicks = barTicks;
  info_.tickSec = tickSamples / sampleRate_;
  info_.bpm = bpm;
  info_.subs = subs_;
  abs_++;
  info_.abs = abs_;
  pollGroups();
  runDue();
  for (const auto& rec : list_) if (on_[static_cast<std::size_t>(rec->group)]) rec->inst->tick();
  subEvent();
}

void NodeSeq::onSub(int index) {
  info_.subIdx = index;
  abs_++;
  info_.abs = abs_;
  runDue();
  subEvent();
}

void NodeSeq::subEvent() {
  for (const auto& rec : list_) if (on_[static_cast<std::size_t>(rec->group)]) rec->inst->sub();
}

void NodeSeq::control(int span) {
  const double dt = span / sampleRate_;
  for (Rec* rec : controlOrder_) if (on_[static_cast<std::size_t>(rec->group)]) rec->inst->control(dt);
}

}  // namespace wax::nodeseq
