// Instrument presets: a snapshot of every parameter of an instrument's nodes, kept on the instrument as
//   presets: [{ id, name, values: { "node.param": number }, data?: { node: its drawings } }]
// The id is what a pattern's command 00 recalls, so it is stable: a new preset gets the lowest free one, and removing a preset leaves the
// others where they are. These functions build the new list; SetPresets (edit/document.h) puts it on the instrument.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "project/project.h"

namespace wax {

// The sound as it is now. `live` gives the running value of "instrument.node.param", or nothing when the engine has none (the value the
// graph holds is used then).
Json capturePreset(const Instrument& instrument, const std::function<std::optional<double>(const std::string&)>& live);

// The drawings the instrument's nodes carry now ({ node: data }, only nodes that have any): kept with a preset beside its values.
Json capturePresetData(const Instrument& instrument);

// The instrument's presets with `values` (and the drawings `data`, when given) added under the lowest free id (named "preset 0A" when `name` is
// empty); `id` gets that id.
Json presetsWithAdded(const Instrument& instrument, const std::string& name, const Json& values, int& id, const Json& data = Json());
Json presetsWithValues(const Instrument& instrument, int id, const Json& values, const Json& data = Json());   // overwrite: id and name stay
Json presetsWithName(const Instrument& instrument, int id, const std::string& name);   // throws when the name is empty
Json presetsWithout(const Instrument& instrument, int id);

std::string presetLabel(int id);   // "0A"

}  // namespace wax
