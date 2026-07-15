#pragma once
#include <string>
#include <memory>
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
    void connect(const std::string& signalingUrl, const std::string& roomCode);

    /// Starts bridging loop.
    void run();

private:
    std::shared_ptr<SignalingClient> signalClient;
    std::shared_ptr<WebRTCSession> session;
    EventQueue eq;
    int winchFd;
    bool running;
};
