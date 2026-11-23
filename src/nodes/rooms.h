// Rooms for the convolver that need no recording: each impulse response is built from the physics of a rectangular room, so every one of
// them sounds like a space that could exist. The direct sound and the first reflections come from the image-source method (the room's walls
// mirror the source), the diffuse tail from noise that fades in three frequency bands (the lows last longest, the highs die with the air and
// the walls), with the tail's energy set by the room's critical distance. The two ears hear different paths, so the response is stereo.
#pragma once

#include <memory>
#include <string>

#include "core/node.h"

namespace wax {

constexpr int kRoomCount = 4;   // rooms 1 to kRoomCount; 0 means "use the sample"

// The room's name, for lists ("studio", "chamber", "hall", "church"); empty for a number outside 1..kRoomCount.
std::string roomName(int room);

// The impulse response of a room at the given sample rate, as the convolver takes a sample (stereo, volume 1). Always the same for the same
// arguments. Returns nullptr for a number outside 1..kRoomCount.
std::shared_ptr<const SampleMessage> makeRoom(int room, double sampleRate);

}  // namespace wax
