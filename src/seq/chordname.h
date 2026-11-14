// Chord names to notes: "F# 13sus", "Eb 9sus4", "E min9(11)", "G Major 7th", "Db 7#9(no5)", "C/E".
// Port of parseChord() and chordNotes() in webtracker src/seq/chordname.js (the syntax is listed there).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace wax {

struct ParsedChord {
  int root = 0;                  // pitch class, 0 to 11
  std::optional<int> bass;       // pitch class of a slash chord's bass note
  std::vector<int> offsets;      // semitones above the root, low to high
  std::vector<int> literal;      // plain notes (">G-4 A-5 C-5"): exactly these pitches; empty for a chord name
};

// Nothing when the text is not a chord this reads.
std::optional<ParsedChord> parseChord(const std::string& text);

// The notes of a chord as note numbers. `octave` is the one of the root (C4 is 60); `inversion` lifts the lowest notes
// an octave; slash chords get the bass below the lowest note; `max` limits the count (the fifth goes first, then the
// highest notes).
std::vector<int> chordNotes(const ParsedChord& chord, int octave = 4, int inversion = 0, int max = 8);

}  // namespace wax
