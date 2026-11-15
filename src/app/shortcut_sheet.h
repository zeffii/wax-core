// The shortcut sheet beside a screen: each key, then what it does, as in the web app's shortcut aside. A screen passes its own list.
#pragma once

#include <vector>

namespace wax::app {

struct Shortcut {
  const char* keys;
  const char* what;
};

// Draws the list in a scrolling child of the given width (0: the rest of the row), with a heading.
void drawShortcutSheet(const char* id, const std::vector<Shortcut>& entries, float width, float height);

// The Pattern screen's keys (only those the native app has).
const std::vector<Shortcut>& patternShortcuts();

}  // namespace wax::app
