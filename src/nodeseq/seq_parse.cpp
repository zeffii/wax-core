#include "nodeseq/seq_parse.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>

#include "core/js_math.h"

namespace wax::nodeseq {
namespace {

// The words of a text, split at spaces and at any of the extra separators.
std::vector<std::string> words(std::string text, const char* separators) {
  for (char& c : text) if (std::string(separators).find(c) != std::string::npos) c = ' ';
  std::istringstream stream(text);
  std::vector<std::string> out;
  for (std::string word; stream >> word;) out.push_back(word);
  return out;
}

// The whole word as a number, the way JS's Number() reads it; nothing when it is not one.
std::optional<double> wholeNumber(const std::string& word) {
  if (word.empty()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(word.c_str(), &end);
  if (end != word.c_str() + word.size() || !std::isfinite(value)) return std::nullopt;
  return value;
}

// "57", "C-5", "C#4", "Db4", "c5" -> a number; "." and "_" are a rest (NaN); anything else is nothing.
std::optional<double> parseValue(const std::string& word) {
  static const std::regex note("^([A-Ga-g])([#b-]?)(-?\\d)$");
  if (word == "." || word == "_") return std::numeric_limits<double>::quiet_NaN();
  if (const auto number = wholeNumber(word)) return number;
  std::smatch m;
  if (!std::regex_match(word, m, note)) return std::nullopt;
  static const int semitone[7] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
  const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(m[1].str()[0])));
  const std::string sign = m[2];
  return std::stoi(m[3]) * 12 + semitone[letter - 'a'] + (sign == "#" ? 1 : sign == "b" ? -1 : 0);
}

std::string trim(const std::string& text) {
  const char* space = " \t\r\n";
  const std::size_t first = text.find_first_not_of(space);
  return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(space) - first + 1);
}

}  // namespace

std::string textOf(const Json& data, const char* key) {
  if (!data.is_object() || !data.contains(key)) return "";
  const Json& value = data[key];
  return value.is_string() ? value.get<std::string>() : value.is_null() ? "" : value.dump();
}

std::vector<double> parseList(const std::string& text) {
  std::vector<double> items;
  for (const std::string& word : words(text, ",")) if (const auto v = parseValue(word)) items.push_back(*v);
  return items;
}

std::vector<double> parseProbs(const std::string& text) {
  std::vector<double> probs;
  std::vector<std::string> list = words(text, ",;");
  if (list.size() > 8) list.resize(8);
  for (const std::string& word : list) {
    char* end = nullptr;
    const double v = std::strtod(word.c_str(), &end);   // (reads the number at the start, as parseFloat does)
    if (end == word.c_str() || !std::isfinite(v)) continue;
    probs.push_back(clamp(word.back() == '%' ? v / 100 : v, 0, 1));
  }
  return probs;
}

std::vector<std::optional<ParsedChord>> parseChordList(const std::string& text) {
  std::vector<std::optional<ParsedChord>> items;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    const std::string name = trim(line);
    if (name.empty()) continue;
    if (name == "." || name == "_") { items.push_back(std::nullopt); continue; }
    if (const auto chord = parseChord(name)) items.push_back(chord);
  }
  return items;
}

ChordText parseChordText(const std::string& text, int octave, int inversion, int max) {
  static const std::regex number("^\\d{1,3}$"), name("^([A-Ga-g])([#b-]?)(-?\\d)$");
  const std::string src = trim(text);
  ChordText out;
  if (src.empty()) return out;

  // the notes themselves, when every word is a note (and the text is not one word that is also a chord name, like "E7")
  struct Written {
    int note;
    std::optional<double> volume;
    std::size_t place;
  };
  const std::vector<std::string> tokens = words(src, ",");
  std::vector<Written> written;
  bool allNotes = true;
  for (std::size_t i = 0; i < tokens.size() && allNotes; i++) {
    const std::size_t at = tokens[i].find('@');
    const std::string noteText = tokens[i].substr(0, at);
    // a note name as the Chord machine reads it: "A2", "C#3", "60" (C-4 is 60)
    std::optional<int> note;
    std::smatch m;
    if (std::regex_match(noteText, number)) { const int n = std::stoi(noteText); if (n <= 127) note = n; }
    else if (std::regex_match(noteText, m, name)) {
      static const int semitone[7] = {9, 11, 0, 2, 4, 5, 7};
      const std::string sign = m[2];
      const int n = (std::stoi(m[3]) + 1) * 12 + semitone[std::tolower(static_cast<unsigned char>(m[1].str()[0])) - 'a'] + (sign == "#" ? 1 : sign == "b" ? -1 : 0);
      if (n >= 0 && n <= 127) note = n;
    }
    if (!note) { allNotes = false; break; }
    std::optional<double> volume;
    if (at != std::string::npos && at + 1 < tokens[i].size()) {
      char* end = nullptr;
      const std::string volumeText = tokens[i].substr(at + 1);
      const double v = std::strtod(volumeText.c_str(), &end);
      if (end == volumeText.c_str() + volumeText.size() && std::isfinite(v)) volume = clamp(v, 0, 100);
    }
    written.push_back({*note, volume, i});
  }
  const bool oneWordChord = tokens.size() == 1 && parseChord(src).has_value();
  if (allNotes && !oneWordChord) {
    std::stable_sort(written.begin(), written.end(), [](const Written& a, const Written& b) { return a.note < b.note; });
    if (static_cast<int>(written.size()) > max) written.resize(static_cast<std::size_t>(std::max(0, max)));
    for (const Written& w : written) { out.notes.push_back(w.note); out.inlineVolumes.push_back(w.volume); }
    return out;
  }

  const auto chord = parseChord(src);
  if (!chord) return out;
  out.notes = chordNotes(*chord, octave, inversion, max);
  std::sort(out.notes.begin(), out.notes.end());
  out.inlineVolumes.assign(out.notes.size(), std::nullopt);
  return out;
}

std::vector<double> voice(std::size_t count, const std::vector<double>& base, double tilt, double variation, double level, const std::function<double()>& rand) {
  std::vector<double> out;
  for (std::size_t i = 0; i < count; i++) {
    double w = (i < base.size() && std::isfinite(base[i]) ? base[i] : 100) / 100;
    const double pos = count > 1 ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.5;
    w *= clamp(1 + tilt * (pos * 2 - 1), 0, 2);
    if (variation > 0) w *= clamp(1 + variation * (rand() * 2 - 1) * 0.7, 0, 1.5);
    out.push_back(clamp(w * level, 0, 1));
  }
  return out;
}

bool euclidHit(int k, double steps, double pulses, double rot) {
  const int s = std::max(1, jsRoundInt(steps)), p = jsRoundInt(pulses);
  const int kk = (((k + jsRoundInt(rot)) % s) + s) % s;
  return (kk * std::max(0, p)) % s < p;
}

}  // namespace wax::nodeseq
