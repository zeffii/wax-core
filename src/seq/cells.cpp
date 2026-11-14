#include "seq/cells.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

namespace wax {
namespace {

const std::array<const char*, 12> kNames = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

float hexCell(const std::string& text, std::size_t digits, double denom) {
  if (text == std::string(digits, '.')) return kEmpty;
  const bool isHex = text.size() == digits && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
  if (!isHex) throw std::runtime_error("bad " + std::to_string(digits) + "-digit hex cell \"" + text + "\"");
  return static_cast<float>(std::stoi(text, nullptr, 16) / denom);
}

}  // namespace

int tokenChannels(Token token) { return token == Token::Cmd ? 2 : 1; }

std::string tokenBlank(Token token) {
  switch (token) {
    case Token::Hex1: return ".";
    case Token::Note: return "...";
    case Token::Hex2: return "..";
    case Token::Hex4: return "....";
    case Token::Cmd: return "......";
    case Token::Spacer: return "|";
  }
  return ".";
}

std::vector<float> encodeCell(const std::string& text, Token token) {
  switch (token) {
    case Token::Note: {
      if (text == "...") return {kEmpty};
      if (text == "^^^") return {kCut};
      if (text == "===") return {kOff};
      int semitone = -1;
      for (std::size_t i = 0; i < kNames.size(); i++) if (text.compare(0, 2, kNames[i]) == 0) semitone = static_cast<int>(i);
      const bool octaveOk = text.size() == 3 && std::isdigit(static_cast<unsigned char>(text[2]));
      const int midi = octaveOk ? (text[2] - '0' + 1) * 12 + semitone : 0;
      if (semitone < 0 || !octaveOk || midi > 127) throw std::runtime_error("bad note \"" + text + "\"");
      return {static_cast<float>(midi / 127.0)};
    }
    case Token::Hex1: return {hexCell(text, 1, 15)};
    case Token::Hex2: return {hexCell(text, 2, 255)};
    case Token::Hex4: return {hexCell(text, 4, 65535)};
    case Token::Spacer: return {kEmpty};
    case Token::Cmd:
      if (text == tokenBlank(Token::Cmd)) return {kEmpty, kEmpty};
      if (text.size() != 6) throw std::runtime_error("bad command cell \"" + text + "\"");
      return {hexCell(text.substr(0, 2), 2, 255), hexCell(text.substr(2), 4, 65535)};
  }
  throw std::runtime_error("unknown cell kind");
}

float relativeNote(double v, int root) {
  const int h = decodeHex2(v);
  return h == 0 ? kOff : static_cast<float>(std::max(0, std::min(127, root + h - 0x80)) / 127.0);
}

}  // namespace wax
