// The code panel of a genexpr node: a text box for its code, a list of templates to start from, what the compiler says about the
// code (compiled, or the mistake with its line and column), and the knobs the code declares. The sound follows the code a moment
// after typing stops, as in the web app. The code is kept in the node's data.code.
#pragma once

#include <string>
#include <vector>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct GenExprWindow {
  bool open = false;
  std::string instrument, node;
  std::vector<char> buffer;        // the text while it is edited
  std::string applied;             // the code the node holds (what the buffer is compared with)
  std::string before;              // the code from before the last template, for "the code I had before"
  double changedAt = 0;            // when the box last changed (the sound follows 280 ms later); 0: nothing waiting
  bool typing = false;             // edits so far belong to one undo step
  int selectFrom = -1, selectTo = -1;   // a range to select in the box (a click on a mistake in the list)
};

void showGenExpr(GenExprWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when the node's code changed (the caller then runs syncData).
bool drawGenExprWindow(GenExprWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app
