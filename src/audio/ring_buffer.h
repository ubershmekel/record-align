#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

// Lock-free ring buffer for exactly one producer thread and one consumer thread.
template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable<T>::value, "SpscRing copies with memcpy");

 public:
  explicit SpscRing(size_t min_capacity = 0) { reset(min_capacity); }

  // Not thread safe. Call before either thread starts using the ring.
  void reset(size_t min_capacity) {
    size_t capacity = 2;
    while (capacity < min_capacity) capacity *= 2;
    buffer_.assign(capacity, T{});
    mask_ = capacity - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  size_t capacity() const { return buffer_.size(); }

  size_t write_available() const {
    return buffer_.size() - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
  }

  size_t read_available() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
  }

  size_t push(const T* src, size_t count) {
    size_t head = head_.load(std::memory_order_relaxed);
    const size_t room = write_available();  // Read once: the consumer can free more at any moment.
    if (count > room) count = room;
    size_t start = head & mask_;
    size_t first = count < buffer_.size() - start ? count : buffer_.size() - start;
    std::memcpy(buffer_.data() + start, src, first * sizeof(T));
    std::memcpy(buffer_.data(), src + first, (count - first) * sizeof(T));
    head_.store(head + count, std::memory_order_release);
    return count;
  }

  size_t push_zeros(size_t count) {
    size_t head = head_.load(std::memory_order_relaxed);
    const size_t room = write_available();
    if (count > room) count = room;
    size_t start = head & mask_;
    size_t first = count < buffer_.size() - start ? count : buffer_.size() - start;
    std::memset(static_cast<void*>(buffer_.data() + start), 0, first * sizeof(T));
    std::memset(static_cast<void*>(buffer_.data()), 0, (count - first) * sizeof(T));
    head_.store(head + count, std::memory_order_release);
    return count;
  }

  size_t pop(T* dst, size_t count) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t available = head_.load(std::memory_order_acquire) - tail;
    if (count > available) count = available;
    size_t start = tail & mask_;
    size_t first = count < buffer_.size() - start ? count : buffer_.size() - start;
    std::memcpy(dst, buffer_.data() + start, first * sizeof(T));
    std::memcpy(dst + first, buffer_.data(), (count - first) * sizeof(T));
    tail_.store(tail + count, std::memory_order_release);
    return count;
  }

 private:
  std::vector<T> buffer_;
  size_t mask_ = 1;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};
