#include "seq/names.h"

#include <cctype>
#include <map>
#include <vector>

namespace wax {

namespace {
const std::map<std::string, std::string>& longTable() {
  static const std::map<std::string, std::string> table = {
#include "seq/names_table.inc"
  };
  return table;
}

bool isVowel(char c) { return std::string("aeiou").find(c) != std::string::npos; }
std::string lowered(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
}  // namespace

std::string shortName(const std::string& label, int width) {
  if (label == "Note" && width >= 3) return "n\xc3\xb6t";   // "nöt": squeezing would read "not"
  if (label == "Smp" && width >= 2) return "id";
  if (label == "Idx" && width >= 2) return "ix";
  if (static_cast<int>(label.size()) <= width || label.empty()) return label;
  const std::string lower = lowered(label);
  if (width <= 1) return lower.substr(0, 1);
  if (std::isdigit(static_cast<unsigned char>(label.back()))) return std::string(1, lower[0]) + label.back();
  std::string picked;
  const std::string rest = lower.substr(1);
  for (char c : rest) if (!isVowel(c) && static_cast<int>(picked.size()) < width - 1) picked += c;
  for (char c : rest) if (static_cast<int>(picked.size()) < width - 1 && picked.find(c) == std::string::npos) picked += c;
  return (std::string(1, lower[0]) + picked).substr(0, static_cast<std::size_t>(width));
}

std::string longName(const std::string& param) {
  const auto found = longTable().find(param);
  if (found != longTable().end()) return found->second;
  std::string words;   // split at a capital or a digit that follows a lower-case letter
  for (std::size_t i = 0; i < param.size(); i++) {
    const char c = param[i];
    if (i > 0 && std::islower(static_cast<unsigned char>(param[i - 1])) && (std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)))) words += ' ';
    words += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (!words.empty()) words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
  return words;
}

}  // namespace wax
