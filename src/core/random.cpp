#include "core/random.h"

namespace wax {

double Xorshift32::next() {
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  return state_ / 4294967296.0;
}

}  // namespace wax
