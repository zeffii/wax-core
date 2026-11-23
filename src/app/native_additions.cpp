#include "app/native_additions.h"

#include <vector>

#include "core/node_registry.h"

namespace wax::app {
namespace {

// A parameter hint (shown in the parameter panel and the node help).
struct ParamNote { const char* type; const char* param; const char* text; };

const std::vector<ParamNote> kParamNotes = {
  {"convolver", "room",
   "0 uses the sample as the response. 1 to 4 are built-in rooms made from room physics (no recording): 1 studio (0.45 s), 2 chamber (1.1 s), 3 hall (2 s), 4 church (4.6 s). Size and decay shape them like any response."},
  {"piano", "realism",
   "0: every note takes a voice. 1: a key struck again while it still rings hits the same strings (one string, one voice), which also saves voices."},
};

// A node put on a wire of a new machine's graph: the wire `from` -> `to` goes through it instead.
struct WireInsert {
  const char* kind;     // the machine kind (machine_table.inc)
  const char* node;     // the node, as JSON
  const char* from;     // "node.port" the wire leaves
  const char* to;       // "node.port" the wire arrives at
};

const std::vector<WireInsert> kWireInserts = {
  // a new piano stands in a hall: one convolution for all the notes, after the voices
  {"piano", R"({"id":"room","type":"convolver","params":{"room":3,"mix":0.2}})", "poly.out", "out.in"},
};

}  // namespace

std::string nativeParamHint(const std::string& type, const std::string& param) {
  for (const ParamNote& note : kParamNotes) if (type == note.type && param == note.param) return note.text;
  return std::string();
}

void applyNativeMachineAdditions(const std::string& kind, Json& instrument) {
  for (const WireInsert& insert : kWireInserts) {
    if (kind != insert.kind) continue;
    Json node = Json::parse(insert.node);
    const NodeType* type = builtinNodes().find(node.value("type", std::string()));
    if (!type || type->inputs.empty() || type->outputs.empty()) continue;   // (a node this engine does not have is left out)
    const std::string id = node.value("id", std::string());
    Json& graph = instrument["graph"];
    bool found = false;
    for (Json& wire : graph["connections"]) {
      if (wire[0] != insert.from || wire[1] != insert.to) continue;
      wire[1] = id + "." + type->inputs.front();
      found = true;
    }
    if (!found) continue;
    graph["connections"].push_back(Json::array({id + "." + type->outputs.front(), insert.to}));
    Json& nodes = graph["nodes"];
    nodes.insert(nodes.end() - 1, node);   // (before the out node, which is last)
  }
}

}  // namespace wax::app
