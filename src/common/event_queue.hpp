#pragma once
#include <cstdint>
#include <queue>
#include <mutex>
#include <string>
#include <memory>

namespace rtc {
class DataChannel;
}

/// Structured thread-safe event.
struct Event {
    enum class Type : std::uint8_t {
        None,  ///< Queue-empty sentinel — not a real event.
        Message,
        Join,
        Disconnect
    };
    Type type;
    std::string clientId;
    std::string clientName;
    std::string data;
    std::shared_ptr<rtc::DataChannel> channel;
};

/// Thread-safe event queue.
class EventQueue {
public:
    /// Creates the queue.
    EventQueue();

    /// Closes eventfd descriptor.
    ~EventQueue();

    /// Pushes message to queue.
    void push(Event ev);

    /// Pops message from queue.
    Event pop();

    /// Gets the eventfd descriptor.
    int getFd() const;

private:
    std::queue<Event> queue;
    mutable std::mutex mutex;
    int efd = -1;
};
