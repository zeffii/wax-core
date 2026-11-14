#include "graph/compile.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "core/js_math.h"

namespace wax {
namespace {

std::runtime_error problem(const std::string& message) { return std::runtime_error(message); }

// "node.port" -> { node, port }
struct Endpoint {
  std::string node;
  std::string port;
};

Endpoint splitEndpoint(const std::string& text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string::npos) return {text, ""};
  const std::size_t next = text.find('.', dot + 1);
  return {text.substr(0, dot), text.substr(dot + 1, next == std::string::npos ? std::string::npos : next - dot - 1)};
}

// A node's input ports: those of its type, then one for each parameter it has been given a port for ("@name").
std::vector<std::string> inputsOf(const GraphNode& node, const NodeType& def) {
  std::vector<std::string> names = def.inputs;
  for (const std::string& p : node.ports) names.push_back("@" + p);
  return names;
}

int indexOf(const std::vector<std::string>& list, const std::string& name) {
  const auto found = std::find(list.begin(), list.end(), name);
  return found == list.end() ? -1 : static_cast<int>(found - list.begin());
}

bool isParamPort(const std::string& name) { return !name.empty() && name[0] == '@'; }

PortKind portKind(const NodeType& def, const std::string& name, const GraphNode& node) {
  if (isParamPort(name)) return PortKind::Param;
  if (node.stereo && indexOf(def.stereoOptional, name) >= 0) return PortKind::Stereo;
  const auto found = def.kinds.find(name);
  return found == def.kinds.end() ? PortKind::Audio : found->second;
}

// ---- range nodes (graph/rangetarget.js) ----

struct RangeTargetInfo {
  const ParamSpec* spec = nullptr;
};

// The first parameter port that a range node's output goes to, or nothing.
std::optional<RangeTarget> rangeTargetOf(const Graph& graph, const NodeRegistry& nodeTypes, const std::string& id) {
  for (const Connection& c : graph.connections) {
    if (c.from != id + ".out") continue;
    const Endpoint to = splitEndpoint(c.to);
    if (!isParamPort(to.port)) continue;
    const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const GraphNode& n) { return n.id == to.node; });
    if (node == graph.nodes.end()) continue;
    const NodeType* def = nodeTypes.find(node->type);
    if (!def) continue;
    const int at = def->paramIndex(to.port.substr(1));
    if (at < 0) continue;
    const ParamSpec& spec = def->params[static_cast<std::size_t>(at)];
    return RangeTarget{spec.min, spec.max, isLogSpec(spec), spec.isInt};
  }
  return std::nullopt;
}

// A range node feeding several ports becomes one copy per wire (ids "id~2" ...; the copies share the original's
// parameters through `alias`), each scaled to its own port. The copy's inputs are wired the same as the original's.
Graph expandRanges(const Graph& source) {
  std::vector<std::pair<std::string, std::vector<Connection>>> outs;
  bool any = false;
  for (const GraphNode& n : source.nodes) {
    if (n.type != "range") continue;
    std::vector<Connection> links;
    for (const Connection& c : source.connections) if (c.from == n.id + ".out") links.push_back(c);
    any = any || links.size() > 1;
    outs.emplace_back(n.id, std::move(links));
  }
  if (!any) return source;

  Graph graph = source;
  for (const auto& [id, links] : outs) {
    if (links.size() < 2) continue;
    const GraphNode original = *std::find_if(source.nodes.begin(), source.nodes.end(), [&](const GraphNode& n) { return n.id == id; });
    for (std::size_t k = 1; k < links.size(); k++) {
      GraphNode copy = original;
      copy.id = id + "~" + std::to_string(k + 1);
      copy.alias = original.alias.empty() ? id : original.alias;
      graph.nodes.push_back(copy);
      const auto wire = std::find_if(graph.connections.begin(), graph.connections.end(),
                                     [&](const Connection& c) { return c.from == links[k].from && c.to == links[k].to; });
      wire->from = copy.id + ".out";
      for (const Connection& c : source.connections) {
        const Endpoint to = splitEndpoint(c.to);
        if (to.node == id) graph.connections.push_back({c.from, copy.id + "." + to.port});
      }
    }
  }
  return graph;
}

// ---- latency (audio/latency.js) ----

int nodeLatency(const GraphNode& node, const NodeType& def, double sampleRate) {
  if (!def.latency) return 0;
  std::map<std::string, double> settings;
  for (const ParamSpec& spec : def.params) settings[spec.name] = node.param(spec.name, spec.def);
  return std::max(0, jsRoundInt(def.latency(settings, sampleRate)));
}

// ---- the compiler ----

struct Link {
  std::string fromId;
  int outPort = 0;
  std::string toId;
  int inPort = 0;
};

struct Entry {
  const GraphNode* node = nullptr;
  const NodeType* def = nullptr;
  std::vector<std::string> inputNames;
};

}  // namespace

CompiledGraph compileGraph(const Graph& source, const std::string& instrument, const NodeRegistry& nodeTypes,
                           ParamRegistry& registry, const CompileOptions& options) {
  const Graph desc = expandRanges(source);

  std::unordered_map<std::string, Entry> byId;
  std::vector<std::string> ids;   // in the order of the description
  for (const GraphNode& n : desc.nodes) {
    if (byId.count(n.id)) throw problem("duplicate node id \"" + n.id + "\"");
    const NodeType* def = nodeTypes.find(n.type);
    if (!def) throw problem("unknown node type \"" + n.type + "\"");
    for (const auto& [name, value] : n.params.items()) {
      if (def->paramIndex(name) < 0) throw problem("node \"" + n.id + "\" has no parameter \"" + name + "\"");
    }
    for (const std::string& name : n.ports) {
      if (def->paramIndex(name) < 0) throw problem("node \"" + n.id + "\" has no parameter \"" + name + "\" to give a port");
    }
    byId[n.id] = {&n, def, inputsOf(n, *def)};
    ids.push_back(n.id);
  }

  std::vector<Link> links;
  for (const Connection& c : desc.connections) {
    const Endpoint from = splitEndpoint(c.from), to = splitEndpoint(c.to);
    const std::string wire = c.from + " -> " + c.to;
    if (!byId.count(from.node)) throw problem("unknown node \"" + from.node + "\" in connection " + wire);
    if (!byId.count(to.node)) throw problem("unknown node \"" + to.node + "\" in connection " + wire);
    const int outPort = indexOf(byId[from.node].def->outputs, from.port), inPort = indexOf(byId[to.node].inputNames, to.port);
    if (outPort < 0) throw problem("\"" + from.node + "\" has no output \"" + from.port + "\"");
    if (inPort < 0) throw problem("\"" + to.node + "\" has no input \"" + to.port + "\"");
    links.push_back({from.node, outPort, to.node, inPort});
  }

  // Several wires into one input are summed (see the sum node, inserted below).
  std::map<std::pair<std::string, int>, std::vector<const Link*>> inputSources;
  for (const Link& l : links) {
    std::vector<const Link*>& sources = inputSources[{l.toId, l.inPort}];
    for (const Link* other : sources) {
      if (other->fromId == l.fromId && other->outPort == l.outPort) throw problem("duplicate connection into \"" + l.toId + "\" port " + std::to_string(l.inPort));
    }
    sources.push_back(&l);
  }

  // Topological order (Kahn). Anything left over is part of a cycle.
  std::unordered_map<std::string, int> indegree;
  for (const std::string& id : ids) indegree[id] = 0;
  for (const Link& l : links) indegree[l.toId]++;
  std::deque<std::string> queue;
  for (const std::string& id : ids) if (indegree[id] == 0) queue.push_back(id);
  std::vector<std::string> order;
  while (!queue.empty()) {
    const std::string id = queue.front();
    queue.pop_front();
    order.push_back(id);
    for (const Link& l : links) {
      if (l.fromId != id) continue;
      if (--indegree[l.toId] == 0) queue.push_back(l.toId);
    }
  }
  if (order.size() != ids.size()) throw problem("graph has a cycle");

  // Which dual nodes run as a stereo pair: those a stereo signal reaches on an audio input (graph/kinds.js).
  std::set<std::string> wide;
  const auto kindOf = [&](const Entry& e, const std::string& port) {
    const PortKind kind = portKind(*e.def, port, *e.node);
    return kind == PortKind::Audio && wide.count(e.node->id) ? PortKind::Stereo : kind;
  };
  for (bool changed = true; changed;) {   // the graph is acyclic, so this settles in at most one pass per node
    changed = false;
    for (const Link& l : links) {
      const Entry& a = byId[l.fromId];
      const Entry& b = byId[l.toId];
      if (!b.def->dual || wide.count(l.toId)) continue;
      const std::string& toPort = b.inputNames[static_cast<std::size_t>(l.inPort)];
      const std::string& fromPort = a.def->outputs[static_cast<std::size_t>(l.outPort)];
      if (portKind(*b.def, toPort, *b.node) == PortKind::Audio && kindOf(a, fromPort) == PortKind::Stereo) { wide.insert(l.toId); changed = true; }
    }
  }

  int nextBuf = 1;
  const auto allocate = [&](bool stereo) {
    BufferRef ref;
    ref.left = nextBuf++;
    if (stereo) ref.right = nextBuf++;
    return ref;
  };
  std::unordered_map<std::string, std::vector<BufferRef>> outBufs;
  for (const std::string& id : order) {
    const Entry& e = byId[id];
    for (const std::string& name : e.def->outputs) outBufs[id].push_back(allocate(kindOf(e, name) == PortKind::Stereo));
  }

  // A node feeding a parameter port starts at where that parameter's slider is (0 to 1 on its scale), so wiring
  // it up does not jump the sound.
  std::unordered_map<std::string, double> hints;
  for (const Link& l : links) {
    const Entry& b = byId[l.toId];
    const std::string& name = b.inputNames[static_cast<std::size_t>(l.inPort)];
    if (!isParamPort(name) || hints.count(l.fromId)) continue;
    const ParamSpec& spec = b.def->params[static_cast<std::size_t>(b.def->paramIndex(name.substr(1)))];
    const double v = b.node->param(spec.name, spec.def);
    const double x = isLogSpec(spec) ? std::log(v / spec.min) / std::log(spec.max / spec.min) : (v - spec.min) / (spec.max - spec.min);
    hints[l.fromId] = clamp(x, 0, 1);
  }

  CompiledGraph compiled;
  compiled.instrument = instrument;
  std::unordered_map<std::string, int> arrive;   // node id -> how many samples late what comes out of it is
  const auto isAudio = [&](const Link& l) {
    const Entry& from = byId[l.fromId];
    const PortKind kind = kindOf(from, from.def->outputs[static_cast<std::size_t>(l.outPort)]);
    return kind != PortKind::Control && kind != PortKind::Param;
  };
  const auto hiddenNode = [&](const std::string& id, const char* type) {
    CompiledNode hidden;
    hidden.id = id;
    hidden.type = type;
    hidden.def = nodeTypes.find(type);
    if (!hidden.def) throw problem(std::string("the engine is missing its \"") + type + "\" node");
    return hidden;
  };

  for (const std::string& id : order) {
    const Entry& e = byId[id];
    const GraphNode& n = *e.node;

    // the audio wires into this node (not the ones into its parameter ports), and the slowest path among them
    std::vector<const Link*> audioIn;
    for (const auto& [key, sources] : inputSources) {
      if (key.first != id) continue;
      for (const Link* l : sources) if (isAudio(*l) && !isParamPort(e.inputNames[static_cast<std::size_t>(l->inPort)])) audioIn.push_back(l);
    }
    int meet = 0;
    for (const Link* l : audioIn) meet = std::max(meet, arrive.count(l->fromId) ? arrive[l->fromId] : 0);
    arrive[id] = meet + nodeLatency(n, *e.def, options.sampleRate);

    CompiledNode node;
    node.id = id;
    node.type = n.type;
    node.def = e.def;
    for (const ParamSpec& p : e.def->params) {
      const std::string address = instrument + "." + (n.alias.empty() ? id : n.alias) + "." + p.name;
      node.slots.push_back(registry.allocate(address, n.param(p.name, p.def), p.min, p.max, p.isInt));
    }

    for (std::size_t i = 0; i < e.inputNames.size(); i++) {
      const bool stereo = kindOf(e, e.inputNames[i]) == PortKind::Stereo;
      std::vector<BufferRef> feeds;
      const auto sources = inputSources.find({id, static_cast<int>(i)});
      if (sources != inputSources.end()) {
        for (std::size_t k = 0; k < sources->second.size(); k++) {
          const Link& l = *sources->second[k];
          BufferRef ref = outBufs[l.fromId][static_cast<std::size_t>(l.outPort)];
          const bool counted = std::find(audioIn.begin(), audioIn.end(), &l) != audioIn.end();
          const int gap = options.delayComp && counted ? meet - (arrive.count(l.fromId) ? arrive[l.fromId] : 0) : 0;
          const std::string tag = id + "." + std::to_string(i) + "." + std::to_string(k);
          if (gap > 0) {   // a quicker path: delayed to match the slowest
            const BufferRef late = allocate(ref.isStereo());
            CompiledNode delay = hiddenNode("~delay." + tag, "compdelay");
            delay.inputs = {ref};
            delay.outputs = {late};
            delay.connected = {true};
            delay.data = {{"samples", gap}};
            delay.stereo = ref.isStereo();
            compiled.nodes.push_back(std::move(delay));
            ref = late;
          }
          if (stereo) {
            feeds.push_back(ref.isStereo() ? ref : BufferRef{ref.left, ref.left});   // a mono wire feeds both channels
          } else if (!ref.isStereo()) {
            feeds.push_back(ref);
          } else {   // a stereo wire into a mono input: averaged
            const BufferRef mono = allocate(false);
            CompiledNode fold = hiddenNode("~fold." + tag, "fold");
            fold.inputs = {ref};
            fold.outputs = {mono};
            fold.connected = {true};
            compiled.nodes.push_back(std::move(fold));
            feeds.push_back(mono);
          }
        }
      }
      if (feeds.empty()) {
        node.inputs.push_back(stereo ? BufferRef{0, 0} : BufferRef{0, -1});
      } else if (feeds.size() == 1) {
        node.inputs.push_back(feeds[0]);
      } else {
        const BufferRef sum = allocate(stereo);
        CompiledNode adder = hiddenNode("~sum." + id + "." + std::to_string(i), stereo ? "sumst" : "sum");
        adder.inputs = feeds;
        adder.outputs = {sum};
        adder.connected.assign(feeds.size(), true);
        compiled.nodes.push_back(std::move(adder));
        node.inputs.push_back(sum);
      }
    }

    // parameter ports: which input carries the value of which parameter, and how a 0 to 1 signal maps onto it
    for (std::size_t k = 0; k < n.ports.size(); k++) {
      const int at = e.def->paramIndex(n.ports[k]);
      const ParamSpec& spec = e.def->params[static_cast<std::size_t>(at)];
      const int input = static_cast<int>(e.def->inputs.size() + k);
      if (!node.inputs[static_cast<std::size_t>(input)].isLive()) continue;
      node.paramPorts.push_back({input, node.slots[static_cast<std::size_t>(at)], spec.min, spec.max, isLogSpec(spec), spec.isInt});
    }

    node.outputs = outBufs[id];
    for (const BufferRef& ref : node.inputs) node.connected.push_back(ref.isLive());
    if (hints.count(id)) node.init = hints[id];
    node.dual = wide.count(id) > 0;
    node.stereo = n.stereo;
    node.source = n.source;
    node.parts = n.parts;
    node.data = n.data;
    if (n.type == "range") node.target = rangeTargetOf(desc, nodeTypes, id);
    compiled.nodes.push_back(std::move(node));
  }

  std::vector<std::string> masters;
  for (const std::string& id : order) if (byId[id].def->master) masters.push_back(id);
  if (masters.size() != 1) throw problem("expected exactly one master node, found " + std::to_string(masters.size()));
  if (desc.voices < 1 || desc.voices > 16) throw problem("voices must be a whole number from 1 to 16, not " + std::to_string(desc.voices));

  const std::string& masterId = masters[0];
  const NodeType& masterDef = *byId[masterId].def;
  const CompiledNode& masterNode = *std::find_if(compiled.nodes.begin(), compiled.nodes.end(), [&](const CompiledNode& x) { return x.id == masterId; });
  const auto slotOfMasterParam = [&](const char* name) {
    const int at = masterDef.paramIndex(name);
    return at < 0 ? -1 : masterNode.slots[static_cast<std::size_t>(at)];
  };

  // The hidden channel stage after the output node: a level (0..1) the sequencer's Volume curve moves.
  compiled.amp = registry.allocate(instrument + ".mix.amp", 1, 0, 1);
  compiled.send = slotOfMasterParam("master");
  compiled.lead = slotOfMasterParam("lead");
  compiled.bufferCount = nextBuf;
  compiled.masterLeft = outBufs[masterId].at(0).left;
  compiled.masterRight = outBufs[masterId].at(1).left;
  compiled.voices = desc.voices;
  compiled.sleeps = !desc.neverSleeps;
  compiled.wired = !masterNode.connected.empty() && masterNode.connected[0];
  compiled.latency = arrive[masterId];
  return compiled;
}

}  // namespace wax
