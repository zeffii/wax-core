// The small formulas behind the room and head nodes, shared with the panels. So far only what the sympathetic strings
// need: the chords, the strings of a chord, and the strings that follow the notes being played.
// Port of the sympathetic strings part of webtracker src/audio/spacecore.js.
#pragma once

#include <vector>

namespace wax::space {

inline constexpr int kMaxStrings = 12;

double midiHz(double midi);

// The pitches of the strings: the chord's notes from the root, climbing through the octaves, `count` of them (MIDI numbers).
std::vector<int> stringsOf(double root, double chord, double count);

// Follow notes: the strings for a bag of notes (MIDI numbers, newest first): the nearest piano string to each of the
// first `partials` harmonics of each note, lowest harmonics and newest notes first, no pitch twice, at most `count`.
std::vector<int> wantedStrings(const std::vector<int>& bag, double partials, double count);

inline constexpr double kSpeed = 343;   // m/s, the speed of sound in air at about 20 C
inline constexpr int kMaxTaps = 32;

// Reflections: the image-source method for a box of a room (w x d x h metres; s and l are the source and the listener, each
// a fraction 0..1 of the floor). The taps come sorted by the way they arrive: delay (s), gain (relative to the direct sound),
// pan (-1 left .. 1 right). `direct` is the delay of the direct sound itself (the dry sound).
struct Tap { double delay, gain, pan; int order; };
struct Room { double direct; std::vector<Tap> list; };
Room reflections(double w, double d, double h, double sx, double sy, double lx, double ly, double absorb, double taps);

}  // namespace wax::space
