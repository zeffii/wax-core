// The test runner. With no arguments it runs every test. Otherwise each argument picks tests, by group or by a piece of "group/name":
//
//   wax-tests                      everything
//   wax-tests --list               the groups and their tests, nothing run
//   wax-tests render-synths        one group
//   wax-tests piano                every test with "piano" in its group or name (the songs pianofelt and pianostrike)
//   wax-tests core edit            several at once
//   wax-tests --quick              everything except the song renders (about 2 seconds; the renders take minutes)
//   wax-tests --times              say how long each test took, slowest last
//   wax-tests --stop               stop after the first test that fails
//
// The old options stay: --it, --xm (the module channels and their imports) and --samples. Exit code: the number of failed checks.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "tests/support.h"

namespace {

using tests::Entry;

// A pick that is the name of a group means that group alone; anything else is a piece of "group/name".
bool matches(const Entry& test, const std::string& pick) {
  const auto& all = tests::allTests();
  const bool isGroup = std::any_of(all.begin(), all.end(), [&](const Entry& e) { return e.group == pick; });
  return isGroup ? test.group == pick : (test.group + "/" + test.name).find(pick) != std::string::npos;
}

void listTests() {
  std::string group;
  for (const Entry& test : tests::allTests()) {
    if (test.group != group) { group = test.group; std::printf("%s\n", group.c_str()); }
    std::printf("  %s\n", test.name.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // (a crash on a CI machine then still shows how far the run got)
  std::vector<std::string> picks;
  bool timed = false, stopAtFirst = false, quick = false;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--list") { listTests(); return 0; }
    else if (arg == "--times") timed = true;
    else if (arg == "--stop") stopAtFirst = true;
    else if (arg == "--quick") quick = true;
    else if (arg == "--it") { picks.push_back("itchan"); picks.push_back("it-import"); }
    else if (arg == "--xm") { picks.push_back("xmchan"); picks.push_back("xm-import"); }
    else if (arg == "--samples") picks.push_back("samples");
    else picks.push_back(arg);
  }

  // the tests are sorted by group, so the lists and the runs read the same way (the order inside a group is the order in the file)
  std::vector<Entry> chosen;
  for (const Entry& test : tests::allTests()) {
    const bool wanted = picks.empty() || std::any_of(picks.begin(), picks.end(), [&](const std::string& pick) { return matches(test, pick); });
    const bool render = test.group.rfind("render-", 0) == 0 || test.group == "threads";
    if (wanted && !(quick && render)) chosen.push_back(test);
  }
  std::stable_sort(chosen.begin(), chosen.end(), [](const Entry& a, const Entry& b) { return a.group < b.group; });
  if (chosen.empty()) { std::printf("no test matches; `wax-tests --list` shows them\n"); return 1; }

  struct Took { std::string label; double seconds; };
  std::vector<Took> took;
  std::vector<std::string> failed;
  for (const Entry& test : chosen) {
    const std::string label = test.group + "/" + test.name;
    std::printf("[%s]\n", label.c_str());   // (so a crash shows where it was)
    const int before = tests::failures;
    const auto started = std::chrono::steady_clock::now();
    try {
      test.run();
    } catch (const std::exception& e) {   // (an exception nobody caught would end the run with no word on why)
      tests::failures++;
      std::printf("FAILED with an exception in %s: %s\n", label.c_str(), e.what());
    }
    took.push_back({label, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()});
    if (tests::failures != before) {
      failed.push_back(label);
      if (stopAtFirst) break;
    }
  }

  if (timed) {
    std::sort(took.begin(), took.end(), [](const Took& a, const Took& b) { return a.seconds < b.seconds; });
    for (const Took& t : took) std::printf("%7.2f s  %s\n", t.seconds, t.label.c_str());
  }
  if (tests::failures) {
    std::printf("%d check%s failed, in:\n", tests::failures, tests::failures == 1 ? "" : "s");
    for (const std::string& label : failed) std::printf("  %s\n", label.c_str());
  } else {
    std::printf("%s tests passed\n", picks.empty() ? "all" : std::to_string(chosen.size()).c_str());
  }
  return tests::failures;
}
