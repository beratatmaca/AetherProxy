#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

/// Lock-free single-producer single-consumer ring.
template <typename T, size_t N>
class SpscQueue {
public:
    /// Pushes one item. False when full.
    bool push(T &&item) {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t next = (t + 1) % N;
        if (next == head.load(std::memory_order_acquire)) {
            return false;
        }
        slots[t] = std::move(item);
        tail.store(next, std::memory_order_release);
        return true;
    }

    /// Pops one item. False when empty.
    bool pop(T &out) {
        size_t h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire)) {
            return false;
        }
        out = std::move(slots[h]);
        head.store((h + 1) % N, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> slots;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
};
