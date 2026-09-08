#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace dedupbackup {

// Fixed-capacity blocking queue: push() blocks while full, pop() blocks
// while empty. This is the backpressure mechanism for the backup
// pipeline's walker/worker split (§3.5): a bounded capacity means a fast
// directory walker cannot race arbitrarily far ahead of the workers and
// accumulate an unbounded number of pending file paths in memory on a
// very large source tree. When the queue is full, push() simply blocks
// until a worker frees a slot — naturally throttling the walker to the
// pace of the slowest consumer. This is deliberately a plain mutex +
// condition_variable design, not a lock-free structure, per the
// project's constraints.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

    // Blocks while the queue is full.
    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return queue_.size() < capacity_; });
        queue_.push_back(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
    }

    // Blocks while the queue is empty and not yet closed. Returns false
    // (leaving `out` untouched) once the queue is closed AND drained —
    // that is the signal for a worker to stop looping and exit.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return false; // must be closed to reach here
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // Called once by the producer (the walker) after it has pushed
    // everything. Wakes any workers blocked in pop() so they can observe
    // the closed queue draining to empty and exit.
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
    }

private:
    size_t capacity_;
    std::deque<T> queue_;
    bool closed_ = false;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};

} // namespace dedupbackup
