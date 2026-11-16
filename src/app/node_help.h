// Node help: what a node is for, how to get the most out of it, where it falls short, then its ports and the hint of each parameter. The
// written pages and the parameter hints come from the web app (tools/gen-node-help.mjs); a node without a written page shows its ports and
// parameters alone. Opened with Shift+F1 on the Instrument screen (or the "?" of the parameter panel's title), in a floating window.
#pragma once

#include <string>

namespace wax::app {

struct NodeHelpWindow {
  bool open = false;
  std::string type;   // the node type shown
};

// The hint of a parameter of a node type ("" when it has none).
std::string paramHint(const std::string& type, const std::string& param);

// Shows the help of a node type in the window.
void showNodeHelp(NodeHelpWindow& window, const std::string& type);

// Draws the window when it is open.
void drawNodeHelp(NodeHelpWindow& window);

}  // namespace wax::app
