#include "app/module_util.h"

#include <cstdio>
#include <stdexcept>

namespace wax::app {

std::string hex(int n, int width) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%0*X", width, n);
  return buffer;
}

std::string cleanName(const std::string& name) {
  std::string id;
  bool gap = false;
  for (const char c : name) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (plain) { if (gap) id += '_'; gap = false; id += c; } else gap = true;
  }
  if (gap) id += '_';
  const std::size_t first = id.find_first_not_of('_'), last = id.find_last_not_of('_');
  id = first == std::string::npos ? std::string() : id.substr(first, last - first + 1);
  if (id.size() > 24) id.resize(24);
  if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos) id = "_" + id;
  return id;
}

std::string noteToken(int semitones) {
  static const char* const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  return std::string(names[semitones % 12]) + std::to_string(semitones / 12);
}

std::string Reader::text(std::size_t from, std::size_t to) const {
  std::string out;
  for (std::size_t i = from; i < to && i < b_.size(); i++) {
    if (b_[i] == 0) break;
    if (b_[i] >= 0x20 && b_[i] <= 0x7e) out += static_cast<char>(b_[i]);
  }
  const std::size_t first = out.find_first_not_of(" \t\r\n"), last = out.find_last_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
}

void Reader::need(std::size_t end) const {
  if (end > b_.size()) throw std::runtime_error("The " + what_ + " stops short.");
}

}  // namespace wax::app
