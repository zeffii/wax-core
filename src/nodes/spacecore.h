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

}  // namespace wax::space
