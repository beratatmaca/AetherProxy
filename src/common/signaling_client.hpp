#pragma once
#include <string>
#include <functional>
#include <memory>
#include <rtc/websocket.hpp>

/// Signaling client using WebSockets.
class SignalingClient {
public:
    /// Creates the client.
    SignalingClient();

    /// Cleans up WebSocket.
    ~SignalingClient();

    /// Connects to signaling server.
    void connect(const std::string& url, const std::string& roomCode);

    /// Sends message to room.
    void send(const std::string& type, const std::string& data);

    /// Sets message reception callback.
    void onMessage(std::function<void(std::string type, std::string data)> cb);

    /// Sets socket open callback.
    void onOpen(std::function<void()> cb);

private:
    std::shared_ptr<rtc::WebSocket> ws;
    std::string room;
    std::function<void(std::string type, std::string data)> messageCallback;
    std::function<void()> openCallback;
};
