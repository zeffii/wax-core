// What the membrane node's picture needs from the node itself, so the picture and the sound agree (membrane.js describeModes and bessel).
#pragma once

#include <vector>

namespace wax {

struct MembraneMode { int m, n; double j, ratio, weight, t60; };   // m nodal lines, n-1 nodal circles; j the Bessel zero

std::vector<MembraneMode> membraneModes(double position, double stiffness, double damping, double highdamp, double quality);
double membraneBessel(int m, double x);
constexpr double kMembraneStrikeAngle = 0.4;

}  // namespace wax
