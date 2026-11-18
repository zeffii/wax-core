// A handle that maps the mouse's position in a rectangle straight onto parameters: across (x) and/or up and down (y).
// Most pictures' draggable dots are this and nothing more, so they are described instead of each getting a drag function:
//   addMapHandle(handles, node, dot, rectMin, rectSize, axis("time", 0, 45, 'l', 0.1), axis("gain", 0, 1.2));
// The rectangle's left edge (top edge, for y) is the first value and its right (bottom) edge the second; give a negative height for
// "up is more". Modes: 'l' linear, 'g' logarithmic, 'b' a level on the echo bars' scale (the top of the rectangle is 1, 54 dB below at the foot).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "app/node_response.h"

namespace wax::app {

// How one axis is written in a handle's parameter name: "name,from,to,mode,step" (step 0: not rounded).
inline std::string axis(const std::string& name, double from, double to, char mode = 'l', double step = 0) {
  std::ostringstream text;
  text << name << ',' << from << ',' << to << ',' << mode << ',' << step;
  return text.str();
}

inline void addMapHandle(ResponseHandles& handles, const GraphNode& node, ImVec2 at, ImVec2 rectMin, ImVec2 rectSize, const std::string& across, const std::string& updown = "") {
  ResponseHandles::Handle handle{node.id, "@map:" + across + ";" + updown, 0, at};
  handle.gx = rectMin.x; handle.gy = rectMin.y; handle.gw = rectSize.x; handle.gh = rectSize.y;
  handles.list.push_back(handle);
}

namespace detail {

inline double mapAxis(const std::string& spec, double fraction, std::string& name) {
  std::vector<std::string> part;
  std::stringstream stream(spec);
  for (std::string item; std::getline(stream, item, ',');) part.push_back(item);
  if (part.size() < 5) return 0;
  name = part[0];
  const double from = std::atof(part[1].c_str()), to = std::atof(part[2].c_str()), step = std::atof(part[4].c_str());
  const double f = std::clamp(fraction, 0.0, 1.0);
  double value = from + (to - from) * f;
  if (part[3] == "g") value = std::exp(std::log(from) + (std::log(to) - std::log(from)) * f);
  else if (part[3] == "b") value = std::pow(10.0, (f * 54 - 54) / 20);
  return step > 0 ? std::round(value / step) * step : value;
}

}  // namespace detail

inline std::vector<std::pair<std::string, double>> dragMapHandle(const ResponseHandles::Handle& handle, const NodeType& type, ImVec2 at) {
  std::vector<std::pair<std::string, double>> edits;
  const std::size_t cut = handle.param.find(';');
  const std::string across = handle.param.substr(5, cut - 5), updown = cut == std::string::npos ? "" : handle.param.substr(cut + 1);
  const auto add = [&](const std::string& spec, double fraction) {
    if (spec.empty()) return;
    std::string name;
    double value = detail::mapAxis(spec, fraction, name);
    for (const ParamSpec& p : type.params) if (p.name == name) value = std::clamp(value, p.min, p.max);
    edits.push_back({name, value});
  };
  add(across, (at.x - handle.gx) / std::max(1e-6f, handle.gw));
  add(updown, (at.y - handle.gy) / (handle.gh == 0 ? 1.0f : handle.gh));
  return edits;
}

}  // namespace wax::app
