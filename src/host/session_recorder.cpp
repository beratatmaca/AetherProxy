#include "host/session_recorder.hpp"
#include <nlohmann/json.hpp>
#include <ctime>
#include <cstdio>

using json = nlohmann::json;

SessionRecorder::SessionRecorder() = default;

SessionRecorder::~SessionRecorder() {
    stop();
}

bool SessionRecorder::start(const std::string &path, uint16_t cols, uint16_t rows) {
    file.open(path, std::ios::trunc);
    if (!file) {
        return false;
    }
    baseline = std::chrono::steady_clock::now();
    json header = {{"version", 2}, {"width", cols}, {"height", rows}, {"timestamp", std::time(nullptr)}};
    file << header.dump() << "\n";
    file.flush();
    running = true;
    drain = std::thread([this]() { drainLoop(); });
    return true;
}

void SessionRecorder::record(std::string_view data) {
    if (!running) {
        return;
    }
    auto offset = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - baseline).count();
    OutputEvent ev{.offset = static_cast<double>(offset) / 1e6, .data = std::string(data)};
    queue.push(std::move(ev));
}

void SessionRecorder::stop() {
    if (!running) {
        return;
    }
    running = false;
    if (drain.joinable()) {
        drain.join();
    }
    file.close();
}

void SessionRecorder::drainLoop() {
    OutputEvent ev;
    while (true) {
        if (queue.pop(ev)) {
            char stamp[32];
            std::snprintf(stamp, sizeof(stamp), "%.6f", ev.offset);
            std::string data = json(ev.data).dump(-1, ' ', false, json::error_handler_t::replace);
            file << "[" << stamp << ", \"o\", " << data << "]\n";
        } else if (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            break;
        }
    }
    file.flush();
}
