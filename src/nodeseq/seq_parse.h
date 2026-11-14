// Reading what is typed into the Node Sequencer's nodes: lists of numbers and notes, lists of chords, a chord with
// volumes, and the volumes themselves.
// Ports of the parsing in webtracker src/nodeseq/nodes.js and src/nodeseq/chordtext.js.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/node.h"   // Json
#include "seq/chordname.h"

namespace wax::nodeseq {

inline constexpr int kMaxChordVoices = 8;

// The numbers in a list's text ("57 60 C-5 . 64"). A dot or an underscore is a rest (NaN); words that cannot be read are left out.
// Here C-5 is 60: the octave times twelve, the way the node's lists have always counted.
std::vector<double> parseList(const std::string& text);

// The probabilities a steps node cycles its steps through ("1.0, 0.3, 60%"): numbers from 0 to 1, at most 8.
std::vector<double> parseProbs(const std::string& text);

// One chord a line: "C maj7", "F# 13sus"; a dot is a rest (nothing). Lines that cannot be read are left out.
std::vector<std::optional<ParsedChord>> parseChordList(const std::string& text);

// A Chord node's text: a chord by name ("A min7"), or the notes themselves ("A2 C3 E3", "57 60 64"), each of which can
// carry its own volume in percent ("A2@100 C3@60"). The notes come out lowest first.
struct ChordText {
  std::vector<int> notes;
  std::vector<std::optional<double>> inlineVolumes;   // percent, for the notes that were written with one
};
ChordText parseChordText(const std::string& text, int octave = 4, int inversion = 0, int max = kMaxChordVoices);

// The volume of each note, 0 to 1. `base` is the volumes in percent by place, tilt runs -1 (the low notes loud) to 1
// (the high ones loud), variation 0 to 1 nudges every note by a random amount, level scales the lot.
std::vector<double> voice(std::size_t count, const std::vector<double>& base, double tilt, double variation, double level, const std::function<double()>& rand);

// Euclid: whether step k of `steps` has a hit when `pulses` are spread out, rotated by `rot`.
bool euclidHit(int k, double steps, double pulses, double rot);

// The text a node's data holds under a key, or "" when it has none.
std::string textOf(const Json& data, const char* key);

}  // namespace wax::nodeseq
