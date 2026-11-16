#include "live/audio_out.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>

#include "miniaudio/miniaudio.h"

namespace wax {

struct AudioOut::Impl {
  ma_context context{};
  ma_device device{};
  bool contextUp = false, deviceUp = false;
  Player* player = nullptr;
  std::atomic<double> load{0};
  std::atomic<long> late{0};
};

static void onAudio(ma_device* device, void* output, const void*, ma_uint32 frames) {
  AudioOut::Impl* impl = static_cast<AudioOut::Impl*>(device->pUserData);
  const auto started = std::chrono::steady_clock::now();
  impl->player->fill(static_cast<float*>(output), static_cast<int>(frames));
  const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const double budget = static_cast<double>(frames) / device->sampleRate;
  const double use = took / budget;
  if (use > 1.0) impl->late.fetch_add(1, std::memory_order_relaxed);
  const double before = impl->load.load(std::memory_order_relaxed);
  impl->load.store(use > before ? use : before * 0.995 + use * 0.005, std::memory_order_relaxed);   // peaks at once, falls slowly
}

AudioOut::AudioOut(Player& player, double sampleRate, bool useNull, int bufferFrames) : impl_(std::make_unique<Impl>()) {
  impl_->player = &player;
  ma_backend nullBackend[] = {ma_backend_null};
  if (ma_context_init(useNull ? nullBackend : nullptr, useNull ? 1 : 0, nullptr, &impl_->context) != MA_SUCCESS)
    throw std::runtime_error("no audio backend could be started");
  impl_->contextUp = true;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 2;
  config.sampleRate = static_cast<ma_uint32>(sampleRate);
  config.dataCallback = onAudio;
  config.pUserData = impl_.get();
  config.periodSizeInFrames = static_cast<ma_uint32>(std::max(0, bufferFrames));
  config.performanceProfile = ma_performance_profile_low_latency;
  if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
    ma_context_uninit(&impl_->context);
    throw std::runtime_error("no audio device could be opened");
  }
  impl_->deviceUp = true;
}

AudioOut::~AudioOut() {
  if (impl_->deviceUp) ma_device_uninit(&impl_->device);
  if (impl_->contextUp) ma_context_uninit(&impl_->context);
}

void AudioOut::start() { ma_device_start(&impl_->device); }
std::string AudioOut::deviceName() const { return impl_->device.playback.name; }
std::string AudioOut::backendName() const { return ma_get_backend_name(impl_->context.backend); }
double AudioOut::sampleRate() const { return impl_->device.sampleRate; }
int AudioOut::bufferFrames() const { return static_cast<int>(impl_->device.playback.internalPeriodSizeInFrames); }
double AudioOut::latencySeconds() const {
  const auto& d = impl_->device;
  return static_cast<double>(d.playback.internalPeriodSizeInFrames) * std::max<ma_uint32>(1, d.playback.internalPeriods) / std::max<ma_uint32>(1, d.sampleRate);
}
double AudioOut::load() const { return impl_->load.load(std::memory_order_relaxed); }
long AudioOut::lateBuffers() const { return impl_->late.load(std::memory_order_relaxed); }

}  // namespace wax
