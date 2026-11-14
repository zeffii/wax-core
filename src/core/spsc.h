// A fixed-size queue for exactly one producer thread and one consumer thread. Neither side ever waits or allocates;
// push() says false when the queue is full (the producer keeps the item and tries again later).
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <utility>

namespace wax {

template <typename T, std::size_t Capacity>
class SpscQueue {
public:
  bool push(T&& item) {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) >= Capacity) return false;
    items_[w % Capacity] = std::move(item);
    write_.store(w + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& item) {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    item = std::move(items_[r % Capacity]);
    items_[r % Capacity] = T();
    read_.store(r + 1, std::memory_order_release);
    return true;
  }

private:
  std::array<T, Capacity> items_{};
  std::atomic<std::size_t> write_{0}, read_{0};
};

}  // namespace wax
