#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace telemetry {
// The I/O thread never waits for processing: a full queue rejects the newest item.
template<class T> class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("Queue capacity must be positive");
    }
    bool try_push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || items_.size() == capacity_) return false;
        items_.push_back(std::move(value));
        high_water_ = std::max(high_water_, items_.size());
        lock.unlock();
        available_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }
    void close() {
        { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; }
        available_.notify_all();
    }
    std::size_t high_water() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_;
    }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<T> items_;
    std::size_t high_water_ = 0;
    bool closed_ = false;
};
}
