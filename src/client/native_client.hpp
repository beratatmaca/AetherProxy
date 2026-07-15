#pragma once
#include <string>
#include <memory>
#include <chrono>
#include <cstdint>
#include "common/config.hpp"
#include "common/webrtc_session.hpp"
#include "common/signaling_client.hpp"
#include "common/event_queue.hpp"

/// Manages native terminal client connection.
class NativeClient {
public:
    /// Creates the client.
    NativeClient();

    /// Cleans up client connections.
    ~NativeClient();

    /// Connects to a remote room.
    void connect(const CLIConfig &config, const std::string &roomCode);

    /// Starts bridging loop.
    void run();

private:
    void applyLocalWinch();
    void renderTelemetry();
    void processOutput(std::string_view data);

    std::shared_ptr<SignalingClient> signalClient;
    std::shared_ptr<WebRTCSession> session;
    EventQueue eq;
    std::string peers;
    std::string myId;
    bool pendingApproval = false;
    std::string lockedBy;
    std::chrono::steady_clock::time_point lockedAt;
    std::string outBuf;
    bool inOsc = false;
    int winchFd = -1;
    bool running = false;
    uint16_t termRows = 0;
    uint16_t termCols = 0;
};
