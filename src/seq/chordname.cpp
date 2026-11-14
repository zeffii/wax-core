#include "seq/chordname.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <set>
#include <sstream>

namespace wax {
namespace {

int letterClass(char letter) {
  switch (std::tolower(static_cast<unsigned char>(letter))) {
    case 'c': return 0;
    case 'd': return 2;
    case 'e': return 4;
    case 'f': return 5;
    case 'g': return 7;
    case 'a': return 9;
    default: return 11;
  }
}

int pitchClass(const std::string& letter, const std::string& sign) {
  return (letterClass(letter[0]) + (sign == "#" ? 1 : sign == "b" ? -1 : 0) + 12) % 12;
}

// Where each degree sits above the root, as written in a plain (major scale) chord; 9, 11 and 13 are an octave up.
int degree(int d) {
  switch (d) {
    case 2: return 2;
    case 3: return 4;
    case 4: return 5;
    case 5: return 7;
    case 6: return 9;
    case 7: return 10;
    case 9: return 14;
    case 11: return 17;
    default: return 21;
  }
}

void replaceAll(std::string& text, const std::string& what, const std::string& with) {
  for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size())) text.replace(at, what.size(), with);
}

// The signs that are not plain letters, written out, so the pieces below only have to read ASCII.
std::string plain(std::string text) {
  replaceAll(text, "♯", "#");         // sharp sign
  replaceAll(text, "♭", "b");         // flat sign
  replaceAll(text, "Δ", "M");         // the major triangle (as the Greek letter)
  replaceAll(text, "△", "M");         // the major triangle (as the shape)
  replaceAll(text, "°", "dim");       // the degree sign
  replaceAll(text, "ø", "halfdim");   // the slashed o
  return text;
}

std::string trim(const std::string& text) {
  const char* space = " \t\r\n";
  const std::size_t first = text.find_first_not_of(space);
  return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(space) - first + 1);
}

std::string dropLeadingGaps(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n,");
  return first == std::string::npos ? "" : text.substr(first);
}

// "C-4", "A2", "Db3", "G#5", "60" -> a MIDI number. C-4 is 60 (middle C), the way the pattern editor writes it.
std::optional<int> parseNoteName(const std::string& token) {
  static const std::regex number("^\\d{1,3}$"), name("^([A-Ga-g])([#b-]?)(-?\\d)$");
  if (std::regex_match(token, number)) {
    const int n = std::stoi(token);
    return n <= 127 ? std::optional<int>(n) : std::nullopt;
  }
  std::smatch m;
  if (!std::regex_match(token, m, name)) return std::nullopt;
  const std::string sign = m[2];
  const int n = (std::stoi(m[3]) + 1) * 12 + letterClass(m[1].str()[0]) + (sign == "#" ? 1 : sign == "b" ? -1 : 0);
  return n >= 0 && n <= 127 ? std::optional<int>(n) : std::nullopt;
}

std::optional<ParsedChord> parsePlainNotes(const std::string& list) {
  std::string spaced = list;
  std::replace(spaced.begin(), spaced.end(), ',', ' ');
  std::istringstream words(spaced);
  std::vector<int> notes;
  for (std::string token; words >> token;) {
    const std::optional<int> n = parseNoteName(token);
    if (!n) return std::nullopt;
    if (std::find(notes.begin(), notes.end(), *n) == notes.end()) notes.push_back(*n);
  }
  if (notes.empty()) return std::nullopt;
  std::sort(notes.begin(), notes.end());
  ParsedChord chord;
  chord.root = notes[0] % 12;
  for (const int n : notes) chord.offsets.push_back(n - notes[0]);
  chord.literal = notes;
  return chord;
}

// What the pieces of a chord name change, one part each.
struct ChordShape {
  int third = 4;
  int fifth = 7;
  std::optional<int> seventh;
  int reach = 0;               // how far the stack of extensions goes: 7, 9, 11 or 13
  bool sixth = false;
  std::vector<int> added;
  std::set<int> omit;
  std::vector<std::pair<int, int>> altered;   // { +1 or -1, degree }
  bool alt = false;
  bool power = false;
  int depth = 0;               // how many brackets are open
};

struct Piece {
  std::regex pattern;
  std::function<void(ChordShape&, const std::smatch&)> apply;
};

int numberOr(const std::smatch& m, std::size_t group, int fallback) { return m[group].matched ? std::stoi(m[group]) : fallback; }

// The pieces, tried in this order; the first that matches the start of what is left is used. Order matters: 'maj' before
// 'm', and the altered notes before the plain numbers.
const std::vector<Piece>& pieces() {
  const auto any = std::regex::ECMAScript, noCase = std::regex::ECMAScript | std::regex::icase;
  const auto reach = [](ChordShape& s, int d) { s.reach = std::max(s.reach, d); };
  const auto add = [](ChordShape& s, int step) { if (std::find(s.added.begin(), s.added.end(), step) == s.added.end()) s.added.push_back(step); };
  static const std::vector<Piece> list = {
    {std::regex("^\\(", any), [](ChordShape& s, const std::smatch&) { s.depth++; }},
    {std::regex("^\\)", any), [](ChordShape& s, const std::smatch&) { s.depth = std::max(0, s.depth - 1); }},
    {std::regex("^(?:no|omit)\\s*(3|5)", noCase), [](ChordShape& s, const std::smatch& m) { s.omit.insert(std::stoi(m[1])); }},
    {std::regex("^(?:add\\s*)?([#b])\\s*(5|9|11|13)(?!\\d)", any), [](ChordShape& s, const std::smatch& m) { s.altered.push_back({m[1] == "#" ? 1 : -1, std::stoi(m[2])}); }},
    {std::regex("^(?:half[- ]?dim(?:inished)?)\\s*(7|9|11|13)?", noCase), [reach](ChordShape& s, const std::smatch& m) { s.third = 3; s.fifth = 6; s.seventh = 10; reach(s, numberOr(m, 1, 7)); }},
    {std::regex("^(?:dim(?:inished)?|o(?![a-zA-Z]))\\s*(7|9|11|13)?", noCase), [reach](ChordShape& s, const std::smatch& m) { s.third = 3; s.fifth = 6; if (m[1].matched) { s.seventh = 9; reach(s, std::stoi(m[1])); } }},
    {std::regex("^(?:aug(?:mented)?|\\+)", noCase), [](ChordShape& s, const std::smatch&) { s.fifth = 8; }},
    {std::regex("^(?:[Mm]ajor|[Mm]aj|MAJ|M)(?![a-z])\\s*(?:(7|9|11|13)(?:th)?)?", any), [reach](ChordShape& s, const std::smatch& m) { if (m[1].matched) { s.seventh = 11; reach(s, std::stoi(m[1])); } }},
    {std::regex("^(?:[Mm]inor|[Mm]in|MIN|[Mm]i|m|-)(?![a-z])", any), [](ChordShape& s, const std::smatch&) { s.third = 3; }},
    {std::regex("^dom(?:inant)?", noCase), [](ChordShape&, const std::smatch&) {}},
    {std::regex("^alt(?:ered)?", noCase), [reach](ChordShape& s, const std::smatch&) { s.alt = true; reach(s, 7); }},
    {std::regex("^sus\\s*2", noCase), [](ChordShape& s, const std::smatch&) { s.third = 2; }},
    {std::regex("^sus\\s*4?", noCase), [](ChordShape& s, const std::smatch&) { s.third = 5; }},
    {std::regex("^6\\s*/\\s*9|^69", any), [add](ChordShape& s, const std::smatch&) { s.sixth = true; add(s, 14); }},
    {std::regex("^add\\s*(2|4|6|9|11|13)", noCase), [add](ChordShape& s, const std::smatch& m) {
      const int d = std::stoi(m[1]);
      add(s, d == 2 ? 14 : d == 4 ? 17 : d == 6 ? 9 : d == 9 ? 14 : d == 11 ? 17 : 21);
    }},
    {std::regex("^(13|11|9|7)(?:th)?", any), [reach, add](ChordShape& s, const std::smatch& m) {
      const int d = std::stoi(m[1]);
      if (!s.depth) reach(s, d);
      else if (d == 7) reach(s, 7);
      else add(s, degree(d));
    }},
    {std::regex("^6(?:th)?", any), [](ChordShape& s, const std::smatch&) { s.sixth = true; }},
    {std::regex("^5(?!\\d)", any), [](ChordShape& s, const std::smatch&) { s.power = true; }},
  };
  return list;
}

}  // namespace

std::optional<ParsedChord> parseChord(const std::string& text) {
  const std::string src = trim(plain(text));
  if (src.empty()) return std::nullopt;
  if (src[0] == '>') return parsePlainNotes(src.substr(1));   // plain notes, not a chord name

  static const std::regex head("^([A-Ga-g])([#b]?)\\s*"), slash("\\s*/\\s*([A-Ga-g])([#b]?)\\s*$");
  std::smatch m;
  if (!std::regex_search(src, m, head)) return std::nullopt;
  ParsedChord chord;
  chord.root = pitchClass(m[1], m[2]);
  std::string rest = src.substr(static_cast<std::size_t>(m.length(0)));
  if (std::regex_search(rest, m, slash)) {
    chord.bass = pitchClass(m[1], m[2]);
    rest = rest.substr(0, static_cast<std::size_t>(m.position(0)));
  }

  ChordShape s;
  for (int guard = 0; !rest.empty() && guard < 60; guard++) {
    rest = dropLeadingGaps(rest);
    if (rest.empty()) break;
    bool taken = false;
    for (const Piece& piece : pieces()) {
      std::smatch found;
      if (!std::regex_search(rest, found, piece.pattern, std::regex_constants::match_continuous)) continue;
      const std::smatch kept = found;
      const std::string after = dropLeadingGaps(rest.substr(static_cast<std::size_t>(found.length(0))));
      piece.apply(s, kept);   // (reads the match before `rest` is replaced)
      rest = after;
      taken = true;
      break;
    }
    if (!taken) return std::nullopt;
  }

  std::set<int> steps = {0};
  if (s.power) s.omit.insert(3);
  if (!s.omit.count(3)) steps.insert(s.third);
  if (!s.omit.count(5)) steps.insert(s.fifth);
  if (s.sixth) steps.insert(9);
  if (s.reach >= 7) steps.insert(s.seventh.value_or(10));
  if (s.reach >= 9) steps.insert(14);
  if (s.reach >= 11) steps.insert(17);
  if (s.reach >= 13) steps.insert(21);
  if (s.reach >= 11 && s.third == 5) steps.erase(17);   // the fourth is already there
  if (s.reach >= 13) steps.erase(17);                    // a 13th chord leaves out the 11th, which fights the third
  if (s.alt) { steps.erase(7); for (const int d : {13, 15, 20}) steps.insert(d); }
  for (const int d : s.added) steps.insert(d);
  for (const auto& [direction, d] : s.altered) {
    if (d == 5) { steps.erase(7); steps.erase(6); steps.erase(8); steps.insert(7 + direction); }
    else { steps.erase(degree(d)); steps.insert(degree(d) + direction); }
  }
  if (s.omit.count(5)) steps.erase(7);
  chord.offsets.assign(steps.begin(), steps.end());
  return chord;
}

std::vector<int> chordNotes(const ParsedChord& chord, int octave, int inversion, int max) {
  const auto liftLowest = [&](std::vector<int>& notes) {
    for (int i = 0; i < inversion && notes.size() > 1; i++) {
      const int lowest = notes.front();
      notes.erase(notes.begin());
      notes.push_back(lowest + 12);
    }
  };
  if (!chord.literal.empty()) {   // plain notes: exactly the pitches typed; too many go from the top
    std::vector<int> notes = chord.literal;
    while (static_cast<int>(notes.size()) > max && notes.size() > 1) notes.pop_back();
    liftLowest(notes);
    std::vector<int> kept;
    for (const int n : notes) if (n >= 0 && n <= 127) kept.push_back(n);
    return kept;
  }
  const int base = 12 * (octave + 1) + chord.root;
  std::vector<int> notes;
  for (const int o : chord.offsets) notes.push_back(base + o);
  while (static_cast<int>(notes.size()) > max && notes.size() > 1) {
    const auto five = std::find(notes.begin(), notes.end(), base + 7);
    notes.erase(five != notes.end() ? five : notes.end() - 1);
  }
  liftLowest(notes);
  if (chord.bass && !notes.empty()) {
    const int below = (notes[0] - *chord.bass + 1200) % 12;
    int b = notes[0] - (below ? below : 12);   // the bass note just below the lowest note
    if (notes[0] - b < 3) b -= 12;             // a second or less below would be muddy: an octave lower
    notes.insert(notes.begin(), b);
  }
  const auto any = [&](auto test) { return std::any_of(notes.begin(), notes.end(), test); };
  while (any([](int n) { return n > 127; })) for (int& n : notes) n -= 12;
  while (any([](int n) { return n < 0; })) for (int& n : notes) n += 12;
  return notes;
}

}  // namespace wax
