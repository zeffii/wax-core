// What the tracker module importers (XM, IT) share: a bounds-checked little-endian reader, hex text, ids and note names.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wax::app {

// Upper-case hex of n, padded with zeros to `width` digits.
std::string hex(int n, int width);

// A typed name made into an id: letters, digits, _ and - only, trimmed, at most 24 characters.
std::string cleanName(const std::string& name);

// The Wax note cell for a note counted in semitones from C-0 ("C-4", "F#2").
std::string noteToken(int semitones);

// A little-endian reader over a file. Reading past the end is an error the user can read: "The <what> stops short."
class Reader {
public:
  Reader(const std::vector<std::uint8_t>& bytes, std::string what) : b_(bytes), what_(std::move(what)) {}
  std::size_t length() const { return b_.size(); }
  int u8(std::size_t at) const { need(at + 1); return b_[at]; }
  int s8(std::size_t at) const { const int v = u8(at); return v > 127 ? v - 256 : v; }
  int u16(std::size_t at) const { need(at + 2); return b_[at] | (b_[at + 1] << 8); }
  std::uint32_t u32(std::size_t at) const {
    need(at + 4);
    return static_cast<std::uint32_t>(b_[at]) | (static_cast<std::uint32_t>(b_[at + 1]) << 8) | (static_cast<std::uint32_t>(b_[at + 2]) << 16) | (static_cast<std::uint32_t>(b_[at + 3]) << 24);
  }
  // The printable characters of a name field, up to its first zero, trimmed.
  std::string text(std::size_t from, std::size_t to) const;
  const std::uint8_t* at(std::size_t i) const { return b_.data() + i; }
  const std::vector<std::uint8_t>& bytes() const { return b_; }

private:
  void need(std::size_t end) const;
  const std::vector<std::uint8_t>& b_;
  std::string what_;
};

}  // namespace wax::app
