// What every test file shares: the check, the way a test registers itself, and the one helper several groups use.
//
//   WAX_TEST("group", testSomething) { check(1 + 1 == 2, "what is being checked"); }
//
// A test needs no list to be added to: defining it registers it. The runner (main.cpp) finds it by its group and its name
// (the function name without "test", in words: testLiveCellText is "live-cell-text").
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tests {

extern int failures;   // the checks that have failed so far

// Records a failed check (and prints it); a passing check says nothing.
void check(bool ok, const std::string& what);

// True when the action throws.
bool throws(const std::function<void()>& action);

struct Entry {
  std::string group, name;
  std::function<void()> run;
};

// Every registered test, in the order they were registered.
const std::vector<Entry>& allTests();

struct Registration {
  Registration(const char* group, const char* functionName, std::function<void()> run);
};

// Adds a test under a name of its own, for the ones made from a table (a song to render).
void add(const std::string& group, const std::string& name, std::function<void()> run);

// Renders tests/songs/<name>.wax and checks it against <name>.ref.json (made by the web app): same length, same peak, nothing unported.
void renderSong(const std::string& name);

}  // namespace tests

#define WAX_TEST(group, function) \
  static void function(); \
  static const ::tests::Registration function##Registration(group, #function, &function); \
  static void function()
