#pragma once
#include <string>
#include <functional>
#include <cstdint>

/// Ephemeral HTTP server for signaling.
class HttpServer {
public:
    /// Creates the HTTP server.
    HttpServer();

    /// Cleans up socket descriptors.
    ~HttpServer();

    /// Starts the HTTP server loop.
    void start(uint16_t port,
               std::function<std::string(std::string)> getOffer,
               std::function<void(std::string, std::string)> onAnswerReceived);

    /// Stops the HTTP server.
    void stop();

private:
    int serverFd;
    bool running;
};
