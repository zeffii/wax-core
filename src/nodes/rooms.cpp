#include "nodes/rooms.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "nodes/shared_dsp.h"

namespace wax {
namespace {

constexpr double kSpeedOfSound = 343;   // metres per second
constexpr int kImageOrder = 3;          // reflections up to this many bounces are placed one by one

struct Room {
  const char* name;
  double length, width, height;   // metres
  double rt60;                    // seconds, at mid frequencies
  double listenerDistance;        // metres from the source to the listener, along the length
};

// Shoebox rooms with real-world sizes and reverberation times (a treated studio, a chamber, a concert hall, a large church).
constexpr std::array<Room, kRoomCount> kRooms = {{
  {"studio", 7.0, 5.5, 3.0, 0.45, 2.6},
  {"chamber", 12.0, 8.5, 5.0, 1.1, 4.0},
  {"hall", 36.0, 22.0, 15.0, 2.0, 7.5},
  {"church", 55.0, 24.0, 22.0, 4.6, 12.0},
}};

// A small repeatable noise: the same room is the same sound every time.
class Noise {
public:
  explicit Noise(std::uint32_t seed) : state_(seed ? seed : 1) {}
  double next() {   // -1 .. 1
    state_ ^= state_ << 13; state_ ^= state_ >> 17; state_ ^= state_ << 5;
    return static_cast<double>(state_) / 2147483648.0 - 1.0;
  }
private:
  std::uint32_t state_;
};

struct Point { double x, y, z; };

double distance(const Point& a, const Point& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// Adds a click of the given height at a fractional position (split between the two samples around it).
void addTap(std::vector<float>& h, double position, double height) {
  const long i = static_cast<long>(std::floor(position));
  const double f = position - static_cast<double>(i);
  if (i >= 0 && i + 1 < static_cast<long>(h.size())) {
    h[static_cast<std::size_t>(i)] += static_cast<float>(height * (1 - f));
    h[static_cast<std::size_t>(i) + 1] += static_cast<float>(height * f);
  }
}

}  // namespace

std::string roomName(int room) { return room >= 1 && room <= kRoomCount ? kRooms[static_cast<std::size_t>(room - 1)].name : std::string(); }

std::shared_ptr<const SampleMessage> makeRoom(int number, double sampleRate) {
  if (number < 1 || number > kRoomCount) return nullptr;
  const Room& room = kRooms[static_cast<std::size_t>(number - 1)];
  const double volume = room.length * room.width * room.height;
  const double surface = 2 * (room.length * room.width + room.length * room.height + room.width * room.height);
  const double meanAbsorption = std::min(0.9, 0.161 * volume / (surface * room.rt60));   // Sabine's formula
  const double reflection = std::sqrt(1 - meanAbsorption);                               // pressure left after one bounce
  const double criticalDistance = 0.057 * std::sqrt(volume / room.rt60);

  const double tailSeconds = std::min(7.5, room.rt60 * 1.25);
  const int length = static_cast<int>(tailSeconds * sampleRate);
  auto message = std::make_shared<SampleMessage>();
  message->rate = sampleRate;
  message->data.assign(static_cast<std::size_t>(length), 0.0f);
  message->right.assign(static_cast<std::size_t>(length), 0.0f);

  // the source stands a third of the way along the room, and the listener is in front of it, a head's width between the ears
  const Point source{room.length * 0.3, room.width * 0.5, 1.0};
  const double earY[2] = {room.width * 0.5 - 0.09, room.width * 0.5 + 0.09};
  const double directDistance = room.listenerDistance;
  double earlyEnergy = 0;
  std::vector<float>* ears[2] = {&message->data, &message->right};
  for (int ear = 0; ear < 2; ear++) {
    const Point listener{source.x + room.listenerDistance, earY[ear], 1.3};
    // image sources: the room repeated in every direction, each copy's source mirrored by the walls it is behind. On one axis, copy n (the pair
    // of walls crossed) and mirror flag q put the image at 2nL + (q ? -x : x) and cost |n - q| + |n| bounces.
    struct Image { double at; int bounces; };
    const auto along = [&](double x, double size) {
      std::vector<Image> images;
      for (int n = -kImageOrder; n <= kImageOrder; n++)
        for (int q = 0; q < 2; q++) {
          const int bounces = std::abs(n - q) + std::abs(n);
          if (bounces <= kImageOrder) images.push_back({2 * n * size + (q ? -x : x), bounces});
        }
      return images;
    };
    for (const Image& ix : along(source.x, room.length))
      for (const Image& iy : along(source.y, room.width))
        for (const Image& iz : along(source.z, room.height)) {
          const int bounces = ix.bounces + iy.bounces + iz.bounces;
          if (bounces > kImageOrder) continue;
          const double d = distance({ix.at, iy.at, iz.at}, listener);
          const double height = std::pow(reflection, bounces) * directDistance / d;   // the direct sound is 1
          addTap(*ears[ear], d / kSpeedOfSound * sampleRate, height);
          if (bounces > 0) earlyEnergy += height * height;
        }
  }
  earlyEnergy /= 2;

  // the diffuse tail: the lows last longer than the mids, the highs die sooner (the air and the walls take them)
  const double bandRt[3] = {room.rt60 * 1.25, room.rt60, room.rt60 * 0.55};
  const double lowCoefficient = 1 - std::exp(-2 * kPi * 250 / sampleRate), highCoefficient = 1 - std::exp(-2 * kPi * 4000 / sampleRate);
  // the reverberant energy is the direct sound's times (distance / critical distance) squared
  const double tailEnergy = std::max(0.25 * directDistance * directDistance / (criticalDistance * criticalDistance), directDistance * directDistance / (criticalDistance * criticalDistance) - earlyEnergy);
  const double mixingTime = std::clamp(0.001 * std::sqrt(volume), 0.01, 0.15);   // seconds: where reflections have become too many to place one by one (about the square root of the volume in ms)
  for (int ear = 0; ear < 2; ear++) {
    Noise noise(0x9e3779b9u * static_cast<std::uint32_t>(number * 2 + ear + 1));
    std::vector<double> tail(static_cast<std::size_t>(length), 0.0);
    double lowState = 0, highState = 0;
    for (int i = 0; i < length; i++) {
      const double white = noise.next();
      lowState += (white - lowState) * lowCoefficient;
      highState += (white - highState) * highCoefficient;
      const double bands[3] = {lowState * 4, (highState - lowState) * 1.6, (white - highState) * 1.2};
      const double t = i / sampleRate;
      double v = 0;
      for (int b = 0; b < 3; b++) v += bands[b] * std::exp(-6.9078 * t / bandRt[b]);
      tail[static_cast<std::size_t>(i)] = v * std::min(1.0, t / mixingTime);   // fades in, so it takes over from the placed reflections
    }
    double energy = 0;
    for (const double v : tail) energy += v * v;
    const double scale = energy > 0 ? std::sqrt(tailEnergy / energy) : 0;
    std::vector<float>& h = *ears[ear];
    for (int i = 0; i < length; i++) h[static_cast<std::size_t>(i)] += static_cast<float>(tail[static_cast<std::size_t>(i)] * scale);
  }
  return message;
}

}  // namespace wax
