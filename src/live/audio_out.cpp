#include "live/audio_out.h"

#include <stdexcept>

#include "miniaudio/miniaudio.h"

namespace wax {

struct AudioOut::Impl {
  ma_context context{};
  ma_device device{};
  bool contextUp = false, deviceUp = false;
};

static void onAudio(ma_device* device, void* output, const void*, ma_uint32 frames) {
  static_cast<Player*>(device->pUserData)->fill(static_cast<float*>(output), static_cast<int>(frames));
}

AudioOut::AudioOut(Player& player, double sampleRate, bool useNull) : impl_(std::make_unique<Impl>()) {
  ma_backend nullBackend[] = {ma_backend_null};
  if (ma_context_init(useNull ? nullBackend : nullptr, useNull ? 1 : 0, nullptr, &impl_->context) != MA_SUCCESS)
    throw std::runtime_error("no audio backend could be started");
  impl_->contextUp = true;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 2;
  config.sampleRate = static_cast<ma_uint32>(sampleRate);
  config.dataCallback = onAudio;
  config.pUserData = &player;
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

}  // namespace wax
