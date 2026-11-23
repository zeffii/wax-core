// Tests: things inside single nodes that a rendered song would not catch on its own.
#include "tests/support.h"

#include <cmath>
#include <vector>

#include "app/machines.h"
#include "app/native_additions.h"
#include "nodes/rooms.h"

using tests::check;

// The built-in rooms ring for the time they are named for, are stereo with two different ears, and are the same every time.
WAX_TEST("nodes", testRoomResponses) {
  const double expected[wax::kRoomCount] = {0.45, 1.1, 2.0, 4.6};   // seconds, as in rooms.cpp
  for (int room = 1; room <= wax::kRoomCount; room++) {
    const auto response = wax::makeRoom(room, 44100);
    check(response && !response->data.empty() && response->data.size() == response->right.size(), wax::roomName(room) + ": a stereo response");
    if (!response) continue;
    const std::size_t n = response->data.size();
    std::vector<double> remaining(n);   // Schroeder's backward integral of the energy
    double total = 0;
    for (std::size_t i = n; i-- > 0;) {
      total += static_cast<double>(response->data[i]) * response->data[i] + static_cast<double>(response->right[i]) * response->right[i];
      remaining[i] = total;
    }
    const auto timeAt = [&](double db) {
      for (std::size_t i = 0; i < n; i++) if (10 * std::log10(remaining[i] / remaining[0]) < db) return static_cast<double>(i) / 44100;
      return 0.0;
    };
    const double rt60 = (timeAt(-25) - timeAt(-5)) * 3;   // from the slope between -5 and -25 dB
    check(std::fabs(rt60 - expected[room - 1]) < 0.12 * expected[room - 1] + 0.03, wax::roomName(room) + ": rings for about its reverberation time");
    check(response->data != response->right, wax::roomName(room) + ": the two ears hear different paths");
    check(wax::makeRoom(room, 44100)->data == response->data, wax::roomName(room) + ": the same every time");
  }
  check(!wax::makeRoom(0, 44100) && !wax::makeRoom(wax::kRoomCount + 1, 44100) && wax::roomName(0).empty(), "numbers outside the rooms give nothing");
}

// A new piano comes with its room after the voices; what native adds does not touch machines that have no addition.
WAX_TEST("nodes", testNativeMachineAdditions) {
  const wax::Json piano = wax::app::machineJson("piano", {});
  bool hasRoom = false, voicesToRoom = false, roomToOut = false, voicesToOut = false;
  for (const wax::Json& node : piano["graph"]["nodes"]) hasRoom |= node["id"] == "room" && node["type"] == "convolver" && node["params"]["room"] == 3;
  for (const wax::Json& wire : piano["graph"]["connections"]) {
    voicesToRoom |= wire[0] == "poly.out" && wire[1] == "room.in";
    roomToOut |= wire[0] == "room.out" && wire[1] == "out.in";
    voicesToOut |= wire[0] == "poly.out" && wire[1] == "out.in";
  }
  check(hasRoom && voicesToRoom && roomToOut && !voicesToOut, "a new piano has a hall after its voices");
  check(piano["graph"]["nodes"].back()["id"] == "out", "the out node stays last");
  const wax::Json kick = wax::app::machineJson("kick", {});
  bool kickHasRoom = false;
  for (const wax::Json& node : kick["graph"]["nodes"]) kickHasRoom |= node["id"] == "room";
  check(!kickHasRoom, "other machines are as the web app has them");
  check(!wax::app::nativeParamHint("convolver", "room").empty() && wax::app::nativeParamHint("convolver", "mix").empty(), "native hints exist only for what the web app lacks");
}
