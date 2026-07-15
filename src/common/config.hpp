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
    /// Offline mode. No external servers.
    bool offline = false;
    /// Ephemeral server port.
    uint16_t port = 8080;
    /// WebSocket signaling server URL. Empty in offline mode.
    std::string signalingUrl;
    /// Seconds without clients before shutdown. Zero disables.
    int idleTimeout = 3600;
};

/// Parses config file then command line flags.
CLIConfig parseCLIArgs(int argc, char *argv[]);

/// Runs the config subcommand. Returns exit code.
int runConfigCommand(const std::vector<std::string> &args);

/// Prints CLI usage text.
void printUsage();

/// Returns the build version string.
std::string versionString();
