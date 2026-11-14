// Pattern cells: the text of a cell <-> the float the sequencer reads. Port of webtracker src/seq/cells.js.
// The sentinels are exact in a float: empty -1, note cut '^^^' -0.75, note off '===' -0.5.
#pragma once

#include <string>
#include <vector>

#include "core/js_math.h"

namespace wax {

inline constexpr float kEmpty = -1.0f, kCut = -0.75f, kOff = -0.5f;

// The kinds of cell text. A Cmd cell is CCXXYY: a two-digit command id on one channel, a four-digit value on the next.
enum class Token { Hex1, Note, Hex2, Hex4, Cmd, Spacer };

int tokenChannels(Token token);        // float channels in the lane's pages
std::string tokenBlank(Token token);   // the text of an empty cell

// One float per channel. Throws std::runtime_error for text that is not a cell of that kind.
std::vector<float> encodeCell(const std::string& text, Token token);

inline int decodeNote(double v) { return jsRoundInt(v * 127); }
inline int decodeHex1(double v) { return jsRoundInt(v * 15); }
inline int decodeHex2(double v) { return jsRoundInt(v * 255); }
inline int decodeHex4(double v) { return jsRoundInt(v * 65535); }

// A relative note cell (a two-digit value, 80 the root note) as the float a note cell holds:
// 00 is a note off, 81 a semitone above the root.
float relativeNote(double v, int root);

}  // namespace wax
