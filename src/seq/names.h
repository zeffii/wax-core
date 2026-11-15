// Column names. A header must fit its cell so the lanes stay narrow: a two-digit column is two characters wide, a one-digit one, one.
// So every column has a short name for the header (lowercase when squeezed) and a long name for the status line under the grid.
// Port of webtracker src/seq/names.js.
#pragma once

#include <string>

namespace wax {

// The label made to fit `width` characters: unchanged if it fits, else its first letter and the first consonants after it
// (Cut -> ct, Res -> rs), or the first letter alone in one character. A trailing digit is kept (Rt2 -> r2).
std::string shortName(const std::string& label, int width);

// "fenv" -> "Filter envelope"; a name that is not listed is split at its capitals: "nnaFade" -> "Nna fade".
std::string longName(const std::string& param);

}  // namespace wax
