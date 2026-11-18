// Where the node pictures (response, envelopes, the parameter-only pictures) are drawn. By default each goes beside its tile in the graph;
// while a slot is set (the node strip does this around each picture) it goes to the slot's top left corner instead, and the caller chooses
// the scale so the picture fits.
#pragma once

#include "imgui.h"

namespace wax::app {

struct PictureSlot {
  bool active = false;
  ImVec2 min{0, 0};
};

inline PictureSlot& pictureSlot() {
  static PictureSlot slot;
  return slot;
}

// The size of a picture in web pixels (multiply by the scale), for the nodes that have one.
struct PictureSize { float w = 0, h = 0; };

inline constexpr PictureSize kResponseSize{320, 190};   // the equaliser's response
inline constexpr PictureSize kEnvelopeSize{204, 86};    // the attack/decay/sustain/release drawing

}  // namespace wax::app
