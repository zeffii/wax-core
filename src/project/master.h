// The master machine: every project gets it when it is loaded, with its lane, first in the lists.
// Port of masterInstrument() and ensureMaster() in webtracker src/instruments.js.
#pragma once

#include "core/node.h"   // Json

namespace wax {

// Works on the file's JSON, before it is read into structs: `data` is the whole project ({ song, instruments, lanes }).
void ensureMaster(Json& data);

}  // namespace wax
