// What the code panel of a genexpr node needs to know about its code: whether it compiles, where it does not, and the knobs it declares.
// (The compiler stops at the first mistake, so there is at most one.)
#pragma once

#include <string>
#include <vector>

namespace wax {

struct GenExprDiagnostic {
  int line = 1, col = 1, end = 2;   // 1-based; `end` is the column after the last character
  std::string message;
};

struct GenExprKnob {
  std::string name;
  double min = 0, max = 1, def = 0;
};

struct GenExprReport {
  bool ok = false;
  std::vector<GenExprDiagnostic> errors;
  std::vector<GenExprKnob> params;   // p1 to p8, in the order they are declared
};

// The code a new genexpr node starts with.
const char* genexprDefaultCode();

GenExprReport checkGenExpr(const std::string& code, double sampleRate = 44100);

}  // namespace wax
