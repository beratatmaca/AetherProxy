#pragma once
#include <string>
#include <vector>
#include <cstdint>

/// Holds command line parameters.
struct CLIConfig {
    /// List of STUN servers.
    std::vector<std::string> stunServers;
    /// List of TURN servers.
    std::vector<std::string> turnServers;
    /// TURN username.
    std::string turnUser;
    /// TURN password.
    std::string turnPass;
    /// Maximum client connections.
    int maxClients = 10;
    /// Disable TURN relay fallback.
    bool noTurn = false;
    /// Disable STUN server fallback.
    bool noStun = false;
    /// Ephemeral server port.
    uint16_t port = 8080;
    /// Optional WebSocket signaling server URL (enables internet mode).
    std::string signalingUrl;
};

/// Parses configuration parameters.
CLIConfig parseCLIArgs(int argc, char* argv[]);
