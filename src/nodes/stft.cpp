#include "nodes/stft.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace wax {

const std::vector<double>& hannWindow(int n) {
  static std::mutex guard;
  static std::map<int, std::unique_ptr<std::vector<double>>> windows;
  const std::lock_guard<std::mutex> lock(guard);
  std::unique_ptr<std::vector<double>>& window = windows[n];
  if (!window) {
    constexpr double pi = 3.141592653589793;
    window = std::make_unique<std::vector<double>>(static_cast<std::size_t>(n));
    for (int i = 0; i < n; i++) (*window)[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos((2 * pi * i) / n);
  }
  return *window;
}

}  // namespace wax
