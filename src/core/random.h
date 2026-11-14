// The engine's dice. webtracker's reference renders (tools/render-ref.mjs) replace Math.random with this same
// xorshift32, from the same seed, so a node that rolls dice can be compared with its JS original.
#pragma once

#include <cstdint>

namespace wax {

inline constexpr std::uint32_t kReferenceSeed = 0x9e3779b9u;

class Xorshift32 {
public:
  explicit Xorshift32(std::uint32_t seed = kReferenceSeed) : state_(seed) {}

  // A number in [0, 1), like Math.random().
  double next();

  void reseed(std::uint32_t seed) { state_ = seed; }

private:
  std::uint32_t state_;
};

}  // namespace wax
