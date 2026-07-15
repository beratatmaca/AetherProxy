#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>

/// Ephemeral HTTP server for signaling.
class HttpServer {
public:
    /// Creates the HTTP server.
    HttpServer();

    /// Cleans up socket descriptors.
    ~HttpServer();

    /// Binds and listens.
    /// @param port Port number. Zero picks a free port.
    /// @param loopbackOnly True binds 127.0.0.1 only.
    bool bindTo(uint16_t port, bool loopbackOnly);

    /// Returns the bound port.
    uint16_t boundPort() const;

    /// Runs the accept loop. Blocks until stop.
    /// @param getOffer Maps client id and display name to an SDP offer. May be null.
    /// @param onAnswerReceived Receives SDP answers. May be null.
    /// @param onControl Receives ping, ended, and bye events.
    void run(const std::function<std::string(std::string, std::string)> &getOffer,
             const std::function<void(std::string, std::string)> &onAnswerReceived,
             const std::function<void(const std::string &)> &onControl = nullptr);

    /// Stops the HTTP server.
    void stop();

private:
    int serverFd = -1;
    uint16_t port = 0;
    std::atomic<bool> running{false};
};
