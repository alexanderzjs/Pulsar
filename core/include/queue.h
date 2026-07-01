#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

namespace pulsar::core {

// Single-producer single-consumer lock-free bounded queue.
// Used between adjacent pipeline stages (capture→encode, encode→transport).
template<typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(size_t capacity)
        : capacity_(capacity + 1), buffer_(capacity + 1)
    {
    }

    // Returns false if queue is full and timeout elapses.
    bool try_push(T item, int timeout_ms = 0) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) % capacity_;
        if (next == tail_.load(std::memory_order_acquire)) {
            if (timeout_ms <= 0) return false;
            std::unique_lock<std::mutex> lk(mtx_);
            not_full_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [&] { return ((head + 1) % capacity_) != tail_.load(std::memory_order_acquire); });
            if (((head + 1) % capacity_) == tail_.load(std::memory_order_acquire)) return false;
        }
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        not_empty_.notify_one();
        return true;
    }

    // Returns false if queue is empty and timeout elapses.
    bool try_pop(T& item, int timeout_ms = 0) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            if (timeout_ms <= 0) return false;
            std::unique_lock<std::mutex> lk(mtx_);
            not_empty_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [&] { return tail_.load(std::memory_order_relaxed) != head_.load(std::memory_order_acquire); });
            tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire)) return false;
        }
        item = std::move(buffer_[tail]);
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        not_full_.notify_one();
        return true;
    }

    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h >= t) ? (h - t) : (capacity_ - t + h);
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::mutex mtx_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
};

} // namespace pulsar::core
