// A node's live picture for the window: the audio thread publishes what the node is doing now, the window thread reads it.
// The window asks again every frame it draws the picture (watch()); a node reports only for a few seconds after the last ask,
// so a picture nobody looks at costs nothing.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace wax {

class SpectrumProbe {
public:
  // Window thread: "I am looking".
  void watch() { until_.store(nowMs() + kWatchMs, std::memory_order_relaxed); }
  // Audio thread: whether anyone is looking.
  bool watched() const { return nowMs() < until_.load(std::memory_order_relaxed); }

  // Audio thread: the latest pictures (values from 0 to 1 along the panel's log axis).
  void publish(const std::vector<float>& input, const std::vector<float>& output) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);   // never wait: the picture can skip one
    if (!lock.owns_lock()) return;
    input_ = input;
    output_ = output;
  }
  // Window thread: false until the first picture has come.
  bool read(std::vector<float>& input, std::vector<float>& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (input_.empty()) return false;
    input = input_;
    output = output_;
    return true;
  }

private:
  static constexpr std::int64_t kWatchMs = 3000;
  static std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  std::atomic<std::int64_t> until_{0};
  std::mutex mutex_;
  std::vector<float> input_, output_;
};

}  // namespace wax
