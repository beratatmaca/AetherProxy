#pragma once
#include <string>
#include <string_view>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include "common/spsc_queue.hpp"

/// Records session output as Asciicast v2.
class SessionRecorder {
public:
    SessionRecorder();

    /// Stops the drain thread.
    ~SessionRecorder();

    /// Opens the file and writes the header. False on failure.
    bool start(const std::string &path, uint16_t cols, uint16_t rows);

    /// Queues one output event. Drops when the queue is full.
    void record(std::string_view data);

    /// Drains remaining events and closes the file.
    void stop();

private:
    struct OutputEvent {
        double offset = 0.0;
        std::string data;
    };

    void drainLoop();

    SpscQueue<OutputEvent, 4096> queue;
    std::ofstream file;
    std::thread drain;
    std::atomic<bool> running{false};
    std::chrono::steady_clock::time_point baseline;
};
