// The small formulas behind the room and head nodes, shared with the panels. So far only what the sympathetic strings
// need: the chords, the strings of a chord, and the strings that follow the notes being played.
// Port of the sympathetic strings part of webtracker src/audio/spacecore.js.
#pragma once

#include <vector>

namespace wax::space {

inline constexpr int kMaxStrings = 12;

// The names of the chords the strings can be tuned to, in the order of the node's `chord` setting.
const std::vector<const char*>& chordNames();

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

// Head pan: how a source at `angle` degrees (0 in front, 90 on the right, 180 behind) and `distance` m reaches the two ears,
// for a head of `head` cm and the head shadow at `shadow` 0..1. `farLeft`: the left ear is the one away from the source;
// itd is how late it is (s), farGain and farCut (Hz) how quiet and dull; rearCut (Hz) the lowpass both ears get from behind.
inline constexpr double kHeadMaxIld = 7;   // dB, the far ear at 90 degrees with the head shadow all the way up
struct Head { bool farLeft; double itd, farGain, farCut, rearCut, gain; };
Head headOf(double angle, double distance, double head, double shadow);

}  // namespace wax::space
