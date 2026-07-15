#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

/// Circular buffer of recent session output.
class ReplayBuffer {
public:
    /// Creates a buffer with fixed byte capacity.
    explicit ReplayBuffer(size_t capacity);

    /// Appends bytes. Oldest bytes drop on overflow.
    void append(std::string_view data);

    /// Returns buffered bytes, oldest first.
    std::string snapshot() const;

private:
    std::vector<char> buf;
    size_t head = 0;
    size_t count = 0;
};
