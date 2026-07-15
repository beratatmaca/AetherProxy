#include "host/replay_buffer.hpp"
#include <algorithm>

ReplayBuffer::ReplayBuffer(size_t capacity) : buf(capacity) {}

void ReplayBuffer::append(std::string_view data) {
    if (buf.empty()) {
        return;
    }
    if (data.size() >= buf.size()) {
        data = data.substr(data.size() - buf.size());
    }
    size_t first = std::min(data.size(), buf.size() - head);
    std::copy(data.begin(), data.begin() + static_cast<long>(first), buf.begin() + static_cast<long>(head));
    std::copy(data.begin() + static_cast<long>(first), data.end(), buf.begin());
    head = (head + data.size()) % buf.size();
    count = std::min(buf.size(), count + data.size());
}

std::string ReplayBuffer::snapshot() const {
    std::string out;
    if (count == 0) {
        return out;
    }
    out.reserve(count);
    size_t start = (head + buf.size() - count) % buf.size();
    size_t first = std::min(count, buf.size() - start);
    out.append(buf.data() + start, first);
    out.append(buf.data(), count - first);
    return out;
}
