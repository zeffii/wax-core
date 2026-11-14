#include "core/params.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace wax {

int ParamRegistry::allocate(const std::string& address, double def, double min, double max, bool isInt) {
  if (const auto known = slots_.find(address); known != slots_.end()) return known->second;
  if (meta_.size() >= kParamCapacity) throw std::runtime_error("parameter block is full");
  const int slot = static_cast<int>(meta_.size());
  slots_.emplace(address, slot);
  meta_.push_back({address, min, max, isInt});
  values_[static_cast<std::size_t>(slot)] = static_cast<float>(def);
  return slot;
}

int ParamRegistry::slotOf(const std::string& address) const {
  const auto found = slots_.find(address);
  return found == slots_.end() ? -1 : found->second;
}

void ParamRegistry::set(const std::string& address, double value) {
  const int slot = slotOf(address);
  if (slot < 0) throw std::runtime_error("unknown parameter \"" + address + "\"");
  if (!std::isfinite(value)) return;
  const ParamMeta& m = meta(slot);
  values_[static_cast<std::size_t>(slot)] = static_cast<float>(std::min(m.max, std::max(m.min, value)));
}

float ParamRegistry::get(const std::string& address) const {
  const int slot = slotOf(address);
  if (slot < 0) throw std::runtime_error("unknown parameter \"" + address + "\"");
  return values_[static_cast<std::size_t>(slot)];
}

}  // namespace wax
