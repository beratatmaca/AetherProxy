#include "common/event_queue.hpp"
#include <sys/eventfd.h>
#include <unistd.h>

EventQueue::EventQueue() {
    efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

EventQueue::~EventQueue() {
    if (efd != -1) {
        close(efd);
    }
}

void EventQueue::push(Event ev) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(std::move(ev));
    }
    uint64_t val = 1;
    eventfd_write(efd, val);
}

Event EventQueue::pop() {
    std::lock_guard<std::mutex> lock(mutex);
    if (queue.empty()) {
        return Event{Event::Type::None, "", "", "", nullptr};
    }
    Event ev = std::move(queue.front());
    queue.pop();
    return ev;
}

int EventQueue::getFd() const {
    return efd;
}
