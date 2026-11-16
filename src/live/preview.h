// Hearing a sound file from the window before it is loaded: one clip at a time, mixed over the engine's output. The window starts and stops it
// and reads how far it has got; the audio thread mixes it in. The two meet at a lock the audio thread only ever tries (a block without the
// preview is better than a block that waits).
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace wax {

struct PreviewClip {
  std::vector<float> left, right;   // the same length; a mono file has both the same
  double rate = 44100;
};

class Preview {
public:
  // Window thread.
  void start(std::shared_ptr<const PreviewClip> clip) {
    std::lock_guard<std::mutex> hold(mutex_);
    clip_ = std::move(clip);
    position_ = 0;
    shared_.store(0, std::memory_order_relaxed);
    playing_.store(clip_ != nullptr && !clip_->left.empty(), std::memory_order_relaxed);
  }
  void stop() {
    std::lock_guard<std::mutex> hold(mutex_);
    playing_.store(false, std::memory_order_relaxed);
  }
  bool playing() const { return playing_.load(std::memory_order_relaxed); }
  double progress() const { return shared_.load(std::memory_order_relaxed); }   // 0..1 through the clip

  // Audio thread: adds `frames` of the clip to interleaved stereo `out`, played at the output's rate (linear interpolation).
  void mixInto(float* out, int frames, double outRate) {
    if (!playing_.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> hold(mutex_, std::try_to_lock);
    if (!hold.owns_lock() || !clip_) return;
    const std::vector<float>& l = clip_->left;
    const std::vector<float>& r = clip_->right;
    const double step = clip_->rate / std::max(1.0, outRate);
    const double length = static_cast<double>(l.size());
    for (int i = 0; i < frames; i++) {
      if (position_ >= length - 1) { playing_.store(false, std::memory_order_relaxed); shared_.store(1, std::memory_order_relaxed); return; }
      const std::size_t at = static_cast<std::size_t>(position_);
      const float frac = static_cast<float>(position_ - static_cast<double>(at));
      out[i * 2] += l[at] + (l[at + 1] - l[at]) * frac;
      out[i * 2 + 1] += r[at] + (r[at + 1] - r[at]) * frac;
      position_ += step;
    }
    shared_.store(position_ / length, std::memory_order_relaxed);
  }

private:
  std::mutex mutex_;
  std::shared_ptr<const PreviewClip> clip_;
  double position_ = 0;
  std::atomic<bool> playing_{false};
  std::atomic<double> shared_{0};
};

}  // namespace wax
