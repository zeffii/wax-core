// What the native app adds to the data it takes from the web app. The machine list and the node help are generated from the web app's
// sources (machine_table.inc, node_help_table.inc: never edited by hand), so anything only native has lives here, in one place, and is
// applied on top. When the web app gets the same thing, the entry here is deleted and the generated data carries it.
#pragma once

#include <string>

#include "core/node.h"

namespace wax::app {

// The hint of a parameter that the web app does not have (or says differently here); "" when there is none. Checked before the generated hints.
std::string nativeParamHint(const std::string& type, const std::string& param);

// Changes a new machine of this kind has in native: nodes put on a wire of its graph. Does nothing for a kind without additions.
void applyNativeMachineAdditions(const std::string& kind, Json& instrument);

}  // namespace wax::app
