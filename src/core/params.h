// The parameter block: one slot per parameter in a flat array of 32-bit floats.
// Loading a graph allocates slots; the sequencer and (later) the UI write them; nodes read them once per block.
// The same idea as src/audio/params.js in webtracker.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace wax {

inline constexpr std::size_t kParamCapacity = 4096;

struct ParamMeta {
  std::string address;   // "instrument.node.param"
  double min = 0;         // the range is kept as doubles, as in JS: a value on a log scale is worked out from them
  double max = 1;
  bool isInt = false;
};

class ParamRegistry {
public:
  // Allocating an address that is already known returns its slot and keeps the current value.
  int allocate(const std::string& address, double def, double min, double max, bool isInt = false);

  // Returns -1 when the address is not known.
  int slotOf(const std::string& address) const;

  // Clamps to the parameter's range. A value that is not finite is ignored (it would silence everything downstream).
  void set(const std::string& address, double value);
  float get(const std::string& address) const;

  const ParamMeta& meta(int slot) const { return meta_[static_cast<std::size_t>(slot)]; }
  std::size_t size() const { return meta_.size(); }

  // What nodes are handed in process(): the value of a parameter is values()[slots[index]].
  float* values() { return values_.data(); }
  const float* values() const { return values_.data(); }

private:
  std::unordered_map<std::string, int> slots_;
  std::vector<ParamMeta> meta_;
  std::array<float, kParamCapacity> values_{};
};

}  // namespace wax
