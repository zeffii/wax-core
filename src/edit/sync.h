// Gets the edits of a Document to the engine that plays it.
#pragma once

#include <string>
#include <vector>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax {

// Sends every lane changed since the last call to the engine, which plays it from the start of its next block. Call it from
// the window thread after edits, undo and redo. Returns a message for each lane that could not be sent (a row the engine cannot
// read): that lane keeps playing as it was, and is tried again at the next call.
std::vector<std::string> syncLanes(Document& document, Engine& engine);

// Sends every parameter changed since the last call to the engine (the value the song now holds for it).
void syncParams(Document& document, Engine& engine);

// Sends every node whose data (drawn envelopes) changed since the last call to the engine.
void syncData(Document& document, Engine& engine);

// Sends every instrument whose wiring changed since the last call to the engine. Returns a message for each one that could not be
// compiled (a loop, say); the caller then undoes the edit that caused it.
std::vector<std::string> syncGraphs(Document& document, Engine& engine);

// Gives every node that plays a sample changed since the last call the new version of it. Returns a message for each one that could not be sent
// (it is tried again at the next call).
std::vector<std::string> syncSamples(Document& document, Engine& engine);

// Sends the song loop to the engine when it changed since the last call.
void syncLoop(Document& document, Engine& engine);

}  // namespace wax
