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
    /// Seconds without clients before shutdown. Zero disables.
    int idleTimeout = 3600;
};

/// Parses config file then command line flags.
CLIConfig parseCLIArgs(int argc, char *argv[]);

/// Prints CLI usage text.
void printUsage();

/// Returns the build version string.
std::string versionString();
