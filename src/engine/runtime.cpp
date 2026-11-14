#include "engine/runtime.h"

#include <algorithm>
#include <cmath>

#include "core/js_math.h"

namespace wax {

namespace {

constexpr double kKillMs = 120;   // how long a kill takes to fade the instrument out before the machine is reset

// A stereo delay line that adds what it hears into a pair of buffers, `gap` samples late (delay compensation between instruments).
class LagLine {
public:
  explicit LagLine(int gap) : gap_(gap), left_(static_cast<std::size_t>(gap), 0.0f), right_(static_cast<std::size_t>(gap), 0.0f) {}

  int gap() const { return gap_; }

  void addInto(float* left, float* right, const float* fromLeft, const float* fromRight, int n) {
    std::size_t at = at_;
    for (int i = 0; i < n; i++) {
      left[i] += left_[at];
      right[i] += right_[at];
      left_[at] = fromLeft[i];
      right_[at] = fromRight[i];
      if (++at == left_.size()) at = 0;
    }
    at_ = at;
  }

private:
  int gap_;
  std::vector<float> left_, right_;
  std::size_t at_ = 0;
};

}  // namespace

// One node of a loaded instrument, with the buffers it reads and writes already looked up.
struct GraphRuntime::RunNode {
  std::string id;
  std::string type;
  std::unique_ptr<Node> node;        // the node itself, or the left copy of a dual pair
  std::unique_ptr<Node> rightNode;   // a dual node on a stereo signal: the copy for the right channel
  std::vector<int> slots;

  std::vector<const float*> inputs;        // flat: one buffer per mono port, two per stereo port
  std::vector<float*> outputs;
  std::vector<const float*> inputsRight;   // dual: the right channel's view of the same ports
  std::vector<float*> outputsRight;

  // a wire into a parameter port sets the parameter, from 0 to 1 on its own scale, before the node runs
  struct PortFeed {
    const float* buffer;
    ParamPort port;
  };
  std::vector<PortFeed> paramFeeds;

  // hard bypass of an effect: the first input goes straight to the first output
  int bypassSlot = -1;
  const float* bypassFrom[2] = {nullptr, nullptr};
  float* bypassTo[2] = {nullptr, nullptr};
  int bypassChannels = 0;

  int voicesTap = -1;   // a voices node of a polyphonic instrument: which tap of the voice sets it adds together

  void sendNote(const NoteEvent& event) {
    node->note(event);
    if (rightNode) rightNode->note(event);
  }
};

// One voice of a polyphonic instrument: its own copy of the per-voice nodes.
struct GraphRuntime::VoiceSet {
  std::vector<std::unique_ptr<RunNode>> nodes;
  std::vector<std::pair<const float*, const float*>> taps;   // what this voice hands to each voices node (or to the mix)
  bool active = false;
  bool released = false;
  int silent = 0;              // blocks of silence in a row
  int note = -1;
  int column = -1;
  std::uint64_t stamp = 0;       // when it started
  std::uint64_t releasedAt = 0;  // when it was let go
};

struct GraphRuntime::Instrument {
  std::string id;
  const CompiledGraph* graph = nullptr;
  std::vector<std::vector<std::vector<float>>> buffers;   // [voice][buffer][sample]; voice 0 also holds the shared part
  std::vector<std::unique_ptr<RunNode>> pre;              // shared nodes that run before the voices
  std::vector<std::unique_ptr<RunNode>> nodes;            // one voice: every node. Several: the part after the voices
  std::vector<VoiceSet> sets;                             // empty with one voice
  std::unordered_map<std::string, std::vector<RunNode*>> byId;   // every copy of a node, by voice

  bool mixing = false;                 // several voices and no voices node: the copies' outputs are added here
  std::vector<float> mixLeft, mixRight;
  float* left = nullptr;               // what the instrument puts out this block
  float* right = nullptr;

  int amp = -1, send = -1, lead = -1;
  double ampPrev = 1;
  int latency = 0;
  std::optional<LagLine> lag;

  std::optional<SleepState> sleep;     // nothing: this machine never sleeps
  double peak = 0;                     // the peak of its last block, for the sidechains that listen to it
  std::int64_t block = -1;             // the last block it ran in
  bool columnVoice = false;

  struct Kill {
    int left;
    int total;
  };
  std::optional<Kill> kill;

  bool arpLooked = false;
  RunNode* arp = nullptr;              // an arp node takes the notes first
  bool keepLooked = false;
  std::vector<RunNode*> keep;          // the free-running modulators outside the voices: they keep time while it sleeps
};

// A listen node that hears the notes of another instrument (see the listen node).
struct GraphRuntime::Listener {
  std::string id;              // the instrument the listen node is in
  std::vector<int> slots;
  std::map<int, int> sent;     // the note heard -> the note that was really played (the transpose may change in between)
};

GraphRuntime::GraphRuntime(double sampleRate, float* params, const NodeRegistry& nodeTypes, int maxBlock)
    : sampleRate_(sampleRate), params_(params), nodeTypes_(nodeTypes), maxBlock_(maxBlock) {
  clock_.tickSamples = (sampleRate * 60) / (120 * 4);
}

GraphRuntime::~GraphRuntime() = default;

GraphRuntime::Instrument* GraphRuntime::find(const std::string& id) {
  const auto found = instruments_.find(id);
  return found == instruments_.end() ? nullptr : found->second.get();
}

// Makes one running node. `own` are the buffers of the voice it belongs to; a buffer written by a shared node is
// read from `shared` (voice 0) instead, so every voice sees the one copy's output.
std::unique_ptr<GraphRuntime::RunNode> GraphRuntime::makeNode(const CompiledNode& compiled, const std::string& instrument, std::vector<std::vector<float>>& own,
                                                             std::vector<std::vector<float>>& shared, const std::set<int>& sharedBuffers) {
  const auto buffer = [&](int index) { return (sharedBuffers.count(index) ? shared : own)[static_cast<std::size_t>(index)].data(); };

  NodeContext context;
  context.sampleRate = sampleRate_;
  context.maxBlock = maxBlock_;
  context.connected = compiled.connected;
  context.stereo = compiled.stereo;
  context.data = compiled.data.is_null() ? nullptr : &compiled.data;
  context.init = compiled.init;
  context.clock = &clock_;
  context.bus = instrument == "master" ? &bus_ : nullptr;
  context.deliver = [this, instrument](const NoteEvent& event) { deliver(instrument, event); };

  auto run = std::make_unique<RunNode>();
  run->id = compiled.id;
  run->type = compiled.type;
  run->slots = compiled.slots;
  run->node = compiled.def->create(context);

  if (compiled.dual) {   // a mono node on a stereo signal: one copy per channel over the same parameters
    run->rightNode = compiled.def->create(context);
    for (const BufferRef& ref : compiled.inputs) {
      run->inputs.push_back(buffer(ref.left));
      run->inputsRight.push_back(buffer(ref.isStereo() ? ref.right : ref.left));
    }
    for (const BufferRef& ref : compiled.outputs) {
      run->outputs.push_back(buffer(ref.left));
      run->outputsRight.push_back(buffer(ref.isStereo() ? ref.right : ref.left));
    }
  } else {
    for (const BufferRef& ref : compiled.inputs) {
      run->inputs.push_back(buffer(ref.left));
      if (ref.isStereo()) run->inputs.push_back(buffer(ref.right));
    }
    for (const BufferRef& ref : compiled.outputs) {
      run->outputs.push_back(buffer(ref.left));
      if (ref.isStereo()) run->outputs.push_back(buffer(ref.right));
    }
  }

  for (const ParamPort& port : compiled.paramPorts) {
    run->paramFeeds.push_back({buffer(compiled.inputs[static_cast<std::size_t>(port.input)].left), port});
  }

  const int bypassAt = compiled.def->paramIndex("bypass");
  if (bypassAt >= 0 && compiled.def->isEffect && !compiled.inputs.empty() && !compiled.outputs.empty()) {
    const BufferRef& from = compiled.inputs[0];
    const BufferRef& to = compiled.outputs[0];
    run->bypassSlot = compiled.slots[static_cast<std::size_t>(bypassAt)];
    run->bypassChannels = to.isStereo() ? 2 : 1;
    run->bypassFrom[0] = buffer(from.left);
    run->bypassFrom[1] = buffer(from.isStereo() ? from.right : from.left);
    run->bypassTo[0] = buffer(to.left);
    run->bypassTo[1] = to.isStereo() ? buffer(to.right) : nullptr;
  }
  return run;
}

// An instrument with several voices runs that many copies of the part of its graph that feeds the `voices` node (or of
// the whole graph, when there is none); the `voices` node adds the copies together and everything after it runs once.
// Notes go to one copy each (see deliver), copies that are not sounding are not run, and parameters are shared by all.
void GraphRuntime::load(const CompiledGraph& source) {
  const std::string id = source.instrument;
  graphs_[id] = std::make_unique<CompiledGraph>(source);
  const CompiledGraph& compiled = *graphs_[id];
  order_.reset();
  followMap_.reset();
  maxDelay_.reset();
  if (std::find(loadOrder_.begin(), loadOrder_.end(), id) == loadOrder_.end()) loadOrder_.push_back(id);

  const int V = std::max(1, std::min(kMaxVoices, compiled.voices));
  const auto fresh = [&] { return std::vector<std::vector<float>>(static_cast<std::size_t>(compiled.bufferCount), std::vector<float>(static_cast<std::size_t>(maxBlock_), 0.0f)); };

  auto inst = std::make_unique<Instrument>();
  inst->id = id;
  inst->graph = &compiled;
  inst->amp = compiled.amp;
  inst->send = compiled.send;
  inst->lead = compiled.lead;
  inst->latency = compiled.latency;
  if (id != "master") inst->sleep = makeSleepState(compiled);   // the master has no voices to wake it: it always runs
  inst->columnVoice = std::any_of(compiled.nodes.begin(), compiled.nodes.end(), [](const CompiledNode& n) { return n.def->columnVoice; });
  inst->buffers.reserve(static_cast<std::size_t>(V));   // so the voices' buffers never move once nodes point at them
  inst->buffers.push_back(fresh());
  std::vector<std::vector<float>>& buffers0 = inst->buffers[0];
  const auto remember = [&](RunNode* node, std::size_t voice) {
    std::vector<RunNode*>& copies = inst->byId[node->id];
    if (copies.size() <= voice) copies.resize(voice + 1, nullptr);
    copies[voice] = node;
  };
  const std::set<int> none;

  if (V == 1) {
    for (const CompiledNode& n : compiled.nodes) {
      inst->nodes.push_back(makeNode(n, id, buffers0, buffers0, none));
      remember(inst->nodes.back().get(), 0);
    }
    inst->left = buffers0[static_cast<std::size_t>(compiled.masterLeft)].data();
    inst->right = buffers0[static_cast<std::size_t>(compiled.masterRight)].data();
    instruments_[id] = std::move(inst);
    return;
  }

  // Which nodes are per voice: those that feed a voices node, or all of them without one.
  // A node type marked `shared` is never copied per voice, even when it feeds something that is: one copy runs first
  // and every voice reads its output, so a sweep is the same for all of them.
  const std::size_t count = compiled.nodes.size();
  const auto isShared = [&](const CompiledNode& n) { return n.def->shared && n.inputs.empty(); };
  const auto buffersOf = [](const std::vector<BufferRef>& refs) {
    std::vector<int> all;
    for (const BufferRef& ref : refs) {
      all.push_back(ref.left);
      if (ref.isStereo()) all.push_back(ref.right);
    }
    return all;
  };
  std::set<int> sharedBuffers;
  std::unordered_map<int, std::size_t> producer;
  std::vector<std::size_t> boundary;   // the voices nodes
  for (std::size_t i = 0; i < count; i++) {
    const CompiledNode& n = compiled.nodes[i];
    if (isShared(n)) for (const int b : buffersOf(n.outputs)) sharedBuffers.insert(b);
    for (const int b : buffersOf(n.outputs)) producer[b] = i;
    if (n.type == "voices") boundary.push_back(i);
  }
  std::set<std::size_t> perVoice;
  const auto visitFeeders = [&](std::size_t start) {
    std::vector<std::size_t> todo = {start};
    while (!todo.empty()) {
      const std::size_t i = todo.back();
      todo.pop_back();
      for (const int b : buffersOf(compiled.nodes[i].inputs)) {
        if (!producer.count(b) || sharedBuffers.count(b)) continue;
        if (perVoice.insert(producer[b]).second) todo.push_back(producer[b]);
      }
    }
  };
  if (!boundary.empty()) for (const std::size_t i : boundary) visitFeeders(i);
  else for (std::size_t i = 0; i < count; i++) perVoice.insert(i);
  for (std::size_t i = 0; i < count; i++) if (isShared(compiled.nodes[i])) perVoice.erase(i);
  for (const std::size_t i : boundary) perVoice.erase(i);   // a voices node is shared, whatever feeds it

  const bool mixing = boundary.empty();
  inst->mixLeft.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
  inst->mixRight.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
  for (int v = 1; v < V; v++) inst->buffers.push_back(fresh());
  inst->sets.resize(static_cast<std::size_t>(V));
  for (std::size_t v = 0; v < static_cast<std::size_t>(V); v++) {
    std::vector<std::vector<float>>& own = inst->buffers[v];
    VoiceSet& set = inst->sets[v];
    const auto at = [&](int b) -> const float* { return (sharedBuffers.count(b) ? inst->buffers[0] : own)[static_cast<std::size_t>(b)].data(); };
    for (std::size_t i = 0; i < count; i++) {
      if (!perVoice.count(i)) continue;
      set.nodes.push_back(makeNode(compiled.nodes[i], id, own, inst->buffers[0], sharedBuffers));
      remember(set.nodes.back().get(), v);
    }
    if (mixing) {
      set.taps.push_back({own[static_cast<std::size_t>(compiled.masterLeft)].data(), own[static_cast<std::size_t>(compiled.masterRight)].data()});
    } else {
      for (const std::size_t i : boundary) {
        const BufferRef& in = compiled.nodes[i].inputs[0];
        set.taps.push_back({at(in.left), at(in.isStereo() ? in.right : in.left)});
      }
    }
  }

  // the shared part, in graph order; a voices node adds the active copies' taps
  for (std::size_t i = 0; i < count; i++) {
    if (perVoice.count(i)) continue;
    const CompiledNode& n = compiled.nodes[i];
    auto node = makeNode(n, id, inst->buffers[0], inst->buffers[0], none);
    RunNode* raw = node.get();
    if (isShared(n)) {
      inst->pre.push_back(std::move(node));
    } else {
      if (n.type == "voices") raw->voicesTap = static_cast<int>(std::find(boundary.begin(), boundary.end(), i) - boundary.begin());
      inst->nodes.push_back(std::move(node));
    }
    remember(raw, 0);
  }
  inst->mixing = mixing;
  inst->left = mixing ? inst->mixLeft.data() : inst->buffers[0][static_cast<std::size_t>(compiled.masterLeft)].data();
  inst->right = mixing ? inst->mixRight.data() : inst->buffers[0][static_cast<std::size_t>(compiled.masterRight)].data();
  instruments_[id] = std::move(inst);
}

void GraphRuntime::runNode(Instrument& inst, RunNode& node, int n) {
  if (node.voicesTap >= 0) {   // the voices node of a polyphonic instrument: the active copies added together
    float* outLeft = node.outputs[0];
    float* outRight = node.outputs[1];
    std::fill(outLeft, outLeft + n, 0.0f);
    std::fill(outRight, outRight + n, 0.0f);
    for (const VoiceSet& set : inst.sets) {
      if (!set.active) continue;
      const auto& [l, r] = set.taps[static_cast<std::size_t>(node.voicesTap)];
      for (int k = 0; k < n; k++) { outLeft[k] += l[k]; outRight[k] += r[k]; }
    }
    return;
  }
  if (node.bypassSlot >= 0 && params_[node.bypassSlot] >= 0.5f) {
    for (int c = 0; c < node.bypassChannels; c++) std::copy(node.bypassFrom[c], node.bypassFrom[c] + n, node.bypassTo[c]);
    return;
  }
  for (const RunNode::PortFeed& feed : node.paramFeeds) {
    const ParamPort& p = feed.port;
    const double x = clamp(feed.buffer[n - 1], 0, 1);
    const double v = p.log ? p.min * std::pow(p.max / p.min, x) : p.min + x * (p.max - p.min);
    if (std::isfinite(v)) params_[p.slot] = static_cast<float>(p.isInt ? jsRound(v) : v);
  }
  node.node->process(node.inputs.data(), node.outputs.data(), n, params_, node.slots.data());
  if (node.rightNode) node.rightNode->process(node.inputsRight.data(), node.outputsRight.data(), n, params_, node.slots.data());
}

// The listen nodes that hear the notes of an instrument.
std::vector<GraphRuntime::Listener>* GraphRuntime::listenersOf(const std::string& id) {
  if (!followMap_) {
    followMap_.emplace();
    for (const std::string& instId : loadOrder_) {
      const CompiledGraph& graph = *graphs_[instId];
      Instrument* inst = find(instId);
      for (const CompiledNode& n : graph.nodes) {
        if (n.type != "listen" || n.source.empty() || !inst) continue;
        const auto copies = inst->byId.find(n.id);
        if (copies == inst->byId.end() || copies->second.empty() || !copies->second[0]) continue;
        (*followMap_)[n.source].push_back({instId, copies->second[0]->slots, {}});
      }
    }
  }
  const auto found = followMap_->find(id);
  return found == followMap_->end() ? nullptr : &found->second;
}

// Passes a note of an instrument on to the instruments that listen to it. The note off finds the note that was
// really played (the transpose may have changed since); with `on` at 0 only the endings still go through.
void GraphRuntime::passOn(const std::string& instrument, const NoteEvent& event) {
  std::vector<Listener>* list = listenersOf(instrument);
  if (!list || following_.count(instrument)) return;
  following_.insert(instrument);
  for (Listener& f : *list) {
    if (following_.count(f.id)) continue;   // it is already passing a note on: a loop closes here
    const bool on = params_[f.slots[0]] >= 0.5f;
    const int shift = jsRoundInt(params_[f.slots[1]]);
    const double scale = params_[f.slots[2]];
    const auto moved = [&](int note) { return std::min(127, std::max(0, note + shift)); };
    NoteEvent out = event;
    if (event.type == NoteType::On) {
      if (!on) continue;
      out.note = moved(event.note);
      out.vel = std::min(1.0, event.vel * scale);
      out.from = event.from >= 0 ? moved(event.from) : event.from;
      f.sent[event.note] = out.note;
    } else if (event.type == NoteType::Off) {
      const auto was = f.sent.find(event.note);
      if (was == f.sent.end() && !on) continue;
      out.note = was == f.sent.end() ? moved(event.note) : was->second;
      f.sent.erase(event.note);
    } else {
      f.sent.clear();
    }
    note(f.id, out);
  }
  following_.erase(instrument);
}

void GraphRuntime::note(const std::string& instrument, const NoteEvent& event) {
  passOn(instrument, event);
  Instrument* inst = find(instrument);
  if (!inst) return;
  if (inst->sleep) inst->sleep->woken = true;
  if (killed_.count(instrument)) {   // a killed machine sleeps until a note-on: then it runs again, from its reset state
    if (event.type != NoteType::On) return;
    killed_.erase(instrument);
  }
  if (inst->kill && event.type == NoteType::On) inst->kill.reset();   // a note arriving during the fade keeps the machine
  if (!inst->arpLooked) {   // an arp node takes the notes first and plays what it makes (see deliver)
    inst->arpLooked = true;
    for (const CompiledNode& n : inst->graph->nodes) {
      if (!n.def->noteFilter) continue;
      const auto copies = inst->byId.find(n.id);
      if (copies != inst->byId.end() && !copies->second.empty()) inst->arp = copies->second[0];
      break;
    }
  }
  if (inst->arp) {
    inst->arp->node->noteInput(event, params_, inst->arp->slots.data());
    return;
  }
  deliver(instrument, event);
}

// The notes as the voices get them. With several voices, a note on takes a silent voice, else the one that was released
// longest ago, else the oldest; a note off releases the newest voice playing that note; all-off releases them all; a cut
// stops them.
void GraphRuntime::deliver(const std::string& instrument, const NoteEvent& event) {
  Instrument* inst = find(instrument);
  if (!inst) return;
  // nodes outside the voices (the note trigger, an envelope that works on the mixed sound) hear every note once, not per voice
  if (!inst->sets.empty()) for (auto& node : inst->pre) node->sendNote(event);
  for (auto& node : inst->nodes) node->sendNote(event);
  if (inst->sets.empty()) return;

  const auto send = [&](VoiceSet& set) { for (auto& node : set.nodes) node->sendNote(event); };
  const auto release = [&](VoiceSet& set) { set.released = true; set.releasedAt = ++stamp_; };
  if (event.type == NoteType::On) {
    // a node type marked columnVoice keeps a note column on one voice: the next note of a column that is still sounding
    // goes to the voice that played the last, which can then glide or slide into it
    VoiceSet* pick = nullptr;
    if (inst->columnVoice && event.from >= 0 && event.column >= 0) {
      for (VoiceSet& s : inst->sets) if (s.active && s.column == event.column) { pick = &s; break; }
    }
    if (!pick) for (VoiceSet& s : inst->sets) if (!s.active) { pick = &s; break; }
    if (!pick) {
      for (VoiceSet& s : inst->sets) {
        const bool better = s.released ? (!pick || !pick->released || s.releasedAt < pick->releasedAt)
                                       : (!(pick && pick->released) && (!pick || s.stamp < pick->stamp));
        if (better) pick = &s;
      }
    }
    pick->active = true;
    pick->released = false;
    pick->silent = 0;
    pick->note = event.note;
    pick->column = event.column;
    pick->stamp = ++stamp_;
    send(*pick);
  } else if (event.type == NoteType::Off) {
    VoiceSet* best = nullptr;
    for (VoiceSet& s : inst->sets) if (s.active && !s.released && s.note == event.note && (!best || s.stamp > best->stamp)) best = &s;
    if (best) { release(*best); send(*best); }
  } else if (event.type == NoteType::AllOff) {
    for (VoiceSet& s : inst->sets) if (s.active && !s.released) { release(s); send(s); }
  } else {   // a cut
    for (VoiceSet& s : inst->sets) { send(s); s.active = false; }
  }
}

void GraphRuntime::setMetaSources(const std::map<std::string, std::vector<std::string>>& map) {
  metaTargets_.clear();
  for (const auto& [id, list] : map) if (!list.empty()) metaTargets_[id] = list;
  order_.reset();
}

// The order the instruments run in: one that listens to another (through a sidechain node) comes after it. Otherwise
// the order they were loaded in. A loop among them is cut where it closes.
const std::vector<std::string>& GraphRuntime::runOrder() {
  if (order_) return *order_;
  std::vector<std::string> order;
  std::set<std::string> seen;
  const std::function<void(const std::string&)> visit = [&](const std::string& id) {
    if (!seen.insert(id).second) return;
    const auto graph = graphs_.find(id);
    if (graph != graphs_.end()) {
      for (const CompiledNode& n : graph->second->nodes) {
        if (n.type != "sidechain" || n.source.empty() || !instruments_.count(n.source)) continue;
        const auto played = metaTargets_.find(n.source);   // a meta machine's sound is its machines': they run first
        if (played != metaTargets_.end()) for (const std::string& t : played->second) if (instruments_.count(t)) visit(t);
        visit(n.source);
      }
    }
    order.push_back(id);
  };
  for (const std::string& id : loadOrder_) visit(id);
  const auto master = std::find(order.begin(), order.end(), "master");   // the master graph (its effects on the summed bus) runs last
  if (master != order.end() && master + 1 != order.end()) std::rotate(master, master + 1, order.end());
  order_ = std::move(order);
  return *order_;
}

void GraphRuntime::kill(const std::string& id) {
  if (!following_.count(id)) {   // a listener is killed with the instrument it listens to
    following_.insert(id);
    if (std::vector<Listener>* list = listenersOf(id)) {
      for (const Listener& f : *list) if (params_[f.slots[0]] >= 0.5f) kill(f.id);
    }
    following_.erase(id);
  }
  Instrument* inst = find(id);
  if (!inst || inst->kill || killed_.count(id)) return;
  const int total = std::max(1, jsRoundInt(kKillMs * sampleRate_ / 1000));
  inst->kill = Instrument::Kill{total, total};
}

void GraphRuntime::setDelayComp(bool on) {
  delayComp_ = on;
  maxDelay_.reset();
}

int GraphRuntime::alignment() {
  if (!delayComp_) return 0;
  if (!maxDelay_) {
    int longest = 0;
    for (const auto& [id, inst] : instruments_) if (id != "master") longest = std::max(longest, inst->latency);
    maxDelay_ = longest;
  }
  return *maxDelay_;
}

// Loads the instrument again from its compiled graph: every node starts from nothing, the parameters stay as they are.
void GraphRuntime::rebuild(const std::string& id) {
  const CompiledGraph copy = *graphs_.at(id);
  load(copy);
  // (the samples and drawn curves a node was sent are replayed here, once node messages are ported)
}

// Asleep this block? Wakes the machine when something could make it sound. A sleeping machine's buffers hold silence.
bool GraphRuntime::dozing(const std::string& id, Instrument& inst, int n) {
  if (!inst.sleep || !inst.sleep->asleep) return false;
  SleepState& z = *inst.sleep;
  const auto peakOf = [this](const std::string& other) { const Instrument* o = find(other); return o ? o->peak : 0.0; };
  if (!sleepIdle_ || inst.kill || shouldWake(z, params_, clock_, peakOf)) {
    z.asleep = false;
    z.quiet = 0;
    z.woken = false;
    return false;
  }
  if (!inst.keepLooked) {
    inst.keepLooked = true;
    for (auto& node : inst.pre) if (isFreeRunning(node->type)) inst.keep.push_back(node.get());
    for (auto& node : inst.nodes) if (isFreeRunning(node->type)) inst.keep.push_back(node.get());
  }
  for (RunNode* node : inst.keep) runNode(inst, *node, n);
  (void)id;
  return true;
}

// After a machine has run: counts its silence, and puts it to sleep once that has lasted long enough with no voice sounding.
void GraphRuntime::settle(Instrument& inst, int n) {
  if (!inst.sleep || !sleepIdle_) return;
  SleepState& z = *inst.sleep;
  z.woken = false;
  const bool sounding = std::any_of(inst.sets.begin(), inst.sets.end(), [](const VoiceSet& s) { return s.active; });
  if (inst.peak >= kSilent || inst.kill || sounding) { z.quiet = 0; return; }
  z.quiet += n;
  if (z.quiet < holdSamples(z, params_, sampleRate_)) return;
  fallAsleep(z, params_, clock_);
  std::fill(inst.left, inst.left + maxBlock_, 0.0f);
  std::fill(inst.right, inst.right + maxBlock_, 0.0f);
  inst.peak = 0;
}

void GraphRuntime::process(float* left, float* right, int n) {
  std::fill(left, left + n, 0.0f);
  std::fill(right, right + n, 0.0f);
  if (!dying_.empty()) {   // the fades that ended last block
    for (const std::string& id : dying_) { rebuild(id); killed_.insert(id); }
    dying_.clear();
  }
  blockNo_++;
  bus_ = {left, right};   // what the master graph's bus-in node hears: everything added so far
  if (masterSlots.carve >= 0 && params_[masterSlots.carve] >= 0.5f) sawUnported_ = true;   // the carve stage would go here

  const std::vector<std::string> order = runOrder();   // (a copy: a rebuild below resets the cached order)
  for (const std::string& id : order) {
    Instrument* found = find(id);
    if (!found) continue;
    if (killed_.count(id)) continue;   // it runs nothing, and its buffers hold silence
    if (dozing(id, *found, n)) { found->block = blockNo_; continue; }   // asleep: nothing runs (a sidechain on it hears silence)
    Instrument& inst = *found;

    for (auto& node : inst.pre) runNode(inst, *node, n);
    if (!inst.sets.empty()) {
      if (inst.mixing) { std::fill(inst.left, inst.left + n, 0.0f); std::fill(inst.right, inst.right + n, 0.0f); }
      for (VoiceSet& set : inst.sets) {
        if (!set.active) continue;
        for (auto& node : set.nodes) runNode(inst, *node, n);
        float peak = 0;
        for (const auto& [l, r] : set.taps) {
          for (int i = 0; i < n; i++) {
            const float a = std::abs(l[i]), b = std::abs(r[i]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
          }
        }
        set.silent = peak < 1e-5 ? set.silent + 1 : 0;
        // finished: it stops costing anything (a node can say it is still in its release, since a pulse train is silent between its pulses)
        const bool busy = std::any_of(set.nodes.begin(), set.nodes.end(), [](const auto& node) { return !node->rightNode && node->node->busy(); });
        if (set.released && set.silent >= 3 && !busy) set.active = false;
        if (inst.mixing) {
          const auto& [l, r] = set.taps[0];
          for (int i = 0; i < n; i++) { inst.left[i] += l[i]; inst.right[i] += r[i]; }
        }
      }
    }
    for (auto& node : inst.nodes) runNode(inst, *node, n);

    if (inst.amp >= 0) {   // the channel stage: the level eased across the block, one multiply per sample
      const double to = params_[inst.amp], from = inst.ampPrev;
      if (from != 1 || to != 1) {
        for (int i = 0; i < n; i++) {
          const double g = from + (to - from) * (i + 1) / n;
          inst.left[i] = static_cast<float>(inst.left[i] * g);
          inst.right[i] = static_cast<float>(inst.right[i] * g);
        }
      }
      inst.ampPrev = to;
    }
    if (inst.kill) {   // a kill in progress: the output eases to nothing across the block
      Instrument::Kill& k = *inst.kill;
      const double from = static_cast<double>(k.left) / k.total, to = static_cast<double>(std::max(0, k.left - n)) / k.total;
      for (int i = 0; i < n; i++) {
        const double g = from + (to - from) * (i + 1) / n;
        inst.left[i] = static_cast<float>(inst.left[i] * g);
        inst.right[i] = static_cast<float>(inst.right[i] * g);
      }
      k.left -= n;
      if (k.left <= 0) { inst.kill.reset(); dying_.insert(id); }
    }

    double sum = 0;
    for (int i = 0; i < n; i++) sum += static_cast<double>(inst.left[i]) + inst.right[i];
    if (!std::isfinite(sum)) {   // one instrument has gone bad: rebuild it, and keep the others sounding
      rebuild(id);
      continue;
    }
    inst.block = blockNo_;
    float peak = 0;
    for (int i = 0; i < n; i++) {
      const float a = std::abs(inst.left[i]), b = std::abs(inst.right[i]);
      if (a > peak) peak = a;
      if (b > peak) peak = b;
    }
    inst.peak = peak;
    settle(inst, n);
    if (inst.sleep && inst.sleep->asleep) continue;                 // it just fell asleep: this block was silent anyway
    if (inst.send >= 0 && params_[inst.send] < 0.5f) continue;      // the out node's master switch is off: only sidechains hear it

    if (id == "master") {
      if (inst.graph->wired) {   // effects on the bus: what the master graph makes replaces the bus
        std::copy(inst.left, inst.left + n, left);
        std::copy(inst.right, inst.right + n, right);
      }
      continue;   // nothing wired into its Out: the bus goes on as it is
    }
    const int gap = alignment() - inst.latency;   // a quicker instrument waits for the slowest
    if (gap > 0) {
      if (!inst.lag || inst.lag->gap() != gap) inst.lag.emplace(gap);
      inst.lag->addInto(left, right, inst.left, inst.right, n);
    } else {
      inst.lag.reset();
      for (int i = 0; i < n; i++) { left[i] += inst.left[i]; right[i] += inst.right[i]; }
    }
  }
}

}  // namespace wax
