#include "common/config.hpp"
#include <string_view>
#include <stdexcept>

CLIConfig parseCLIArgs(int argc, char* argv[]) {
    CLIConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--stun" && i + 1 < argc) {
            config.stunServers.push_back(argv[++i]);
        } else if (arg == "--turn" && i + 1 < argc) {
            config.turnServers.push_back(argv[++i]);
        } else if (arg == "--turn-user" && i + 1 < argc) {
            config.turnUser = argv[++i];
        } else if (arg == "--turn-pass" && i + 1 < argc) {
            config.turnPass = argv[++i];
        } else if (arg == "--max-clients" && i + 1 < argc) {
            config.maxClients = std::stoi(argv[++i]);
        } else if (arg == "--no-turn") {
            config.noTurn = true;
        } else if (arg == "--no-stun") {
            config.noStun = true;
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--signal" && i + 1 < argc) {
            config.signalingUrl = argv[++i];
        }
    }
    return config;
}
