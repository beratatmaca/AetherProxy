#include "common/config.hpp"
#include <string_view>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cctype>

#ifndef AETHER_VERSION
#define AETHER_VERSION "dev"
#endif

static std::string extractQuoted(std::string_view text, size_t &pos) {
    size_t open = text.find('"', pos);
    if (open == std::string_view::npos) {
        pos = std::string_view::npos;
        return "";
    }
    size_t close = text.find('"', open + 1);
    if (close == std::string_view::npos) {
        pos = std::string_view::npos;
        return "";
    }
    pos = close + 1;
    return std::string(text.substr(open + 1, close - open - 1));
}

static size_t findKey(std::string_view text, std::string_view key) {
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string_view::npos) {
        bool startOk = pos == 0 || text[pos - 1] == '\n' || text[pos - 1] == ' ' || text[pos - 1] == '\t' || text[pos - 1] == '{' ||
                       text[pos - 1] == ',';
        size_t after = pos + key.size();
        while (after < text.size() && (text[after] == ' ' || text[after] == '\t')) {
            ++after;
        }
        bool eqOk = after < text.size() && text[after] == '=';
        if (startOk && eqOk) {
            return after + 1;
        }
        pos += key.size();
    }
    return std::string_view::npos;
}

static std::string_view sectionText(std::string_view content, std::string_view header) {
    size_t start = content.find(header);
    if (start == std::string_view::npos) {
        return {};
    }
    start += header.size();
    size_t end = content.find("\n[", start);
    if (end == std::string_view::npos) {
        end = content.size();
    }
    return content.substr(start, end - start);
}

static long parseNumberAt(std::string_view text, size_t pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    size_t end = pos;
    while (end < text.size() && (std::isdigit(text[end]) != 0)) {
        ++end;
    }
    if (end == pos) {
        return -1;
    }
    return std::strtol(std::string(text.substr(pos, end - pos)).c_str(), nullptr, 10);
}

static void loadConfigFile(CLIConfig &config) {
    const char *home = std::getenv("HOME");
    if (home == nullptr) {
        return;
    }
    std::ifstream file(std::string(home) + "/.config/aetherproxy/config.toml");
    if (!file) {
        return;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();

    std::string_view ice = sectionText(content, "[ice]");
    size_t stunPos = findKey(ice, "stun");
    if (stunPos != std::string_view::npos) {
        size_t open = ice.find('[', stunPos);
        size_t close = ice.find(']', open);
        if (open != std::string_view::npos && close != std::string_view::npos) {
            std::string_view arr = ice.substr(open, close - open);
            size_t pos = 0;
            while (pos != std::string_view::npos) {
                std::string value = extractQuoted(arr, pos);
                if (!value.empty()) {
                    config.stunServers.push_back(value);
                }
            }
        }
    }
    size_t turnPos = findKey(ice, "turn");
    if (turnPos != std::string_view::npos) {
        size_t open = ice.find('[', turnPos);
        size_t close = ice.find(']', open);
        if (open != std::string_view::npos && close != std::string_view::npos) {
            std::string_view arr = ice.substr(open, close - open);
            size_t blockPos = 0;
            while ((blockPos = arr.find('{', blockPos)) != std::string_view::npos) {
                size_t blockEnd = arr.find('}', blockPos);
                if (blockEnd == std::string_view::npos) {
                    break;
                }
                std::string_view block = arr.substr(blockPos, blockEnd - blockPos);
                size_t p = findKey(block, "url");
                if (p != std::string_view::npos) {
                    config.turnServers.push_back(extractQuoted(block, p));
                }
                p = findKey(block, "username");
                if (p != std::string_view::npos) {
                    config.turnUser = extractQuoted(block, p);
                }
                p = findKey(block, "credential");
                if (p != std::string_view::npos) {
                    config.turnPass = extractQuoted(block, p);
                }
                blockPos = blockEnd + 1;
            }
        }
    }

    std::string_view session = sectionText(content, "[session]");
    size_t keyPos = findKey(session, "idle_timeout");
    if (keyPos != std::string_view::npos) {
        long v = parseNumberAt(session, keyPos);
        if (v >= 0) {
            config.idleTimeout = static_cast<int>(v);
        }
    }
    keyPos = findKey(session, "max_clients");
    if (keyPos != std::string_view::npos) {
        long v = parseNumberAt(session, keyPos);
        if (v > 0) {
            config.maxClients = static_cast<int>(v);
        }
    }
    keyPos = findKey(session, "port");
    if (keyPos != std::string_view::npos) {
        long v = parseNumberAt(session, keyPos);
        if (v > 0 && v < 65536) {
            config.port = static_cast<uint16_t>(v);
        }
    }
}

CLIConfig parseCLIArgs(int argc, char *argv[]) {
    CLIConfig config;
    loadConfigFile(config);

    std::vector<std::string> cliStun;
    std::vector<std::string> cliTurn;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--stun" && i + 1 < argc) {
            cliStun.emplace_back(argv[++i]);
        } else if (arg == "--turn" && i + 1 < argc) {
            cliTurn.emplace_back(argv[++i]);
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
        } else if (arg == "--idle-timeout" && i + 1 < argc) {
            config.idleTimeout = std::stoi(argv[++i]);
        }
    }

    if (!cliStun.empty()) {
        config.stunServers = std::move(cliStun);
    }
    if (!cliTurn.empty()) {
        config.turnServers = std::move(cliTurn);
    }

    if (config.stunServers.empty() && !config.noStun) {
        config.stunServers.emplace_back("stun:stun.l.google.com:19302");
    }
    if (config.turnServers.empty() && config.turnUser.empty() && !config.noTurn) {
        config.turnServers.emplace_back("turn:openrelay.metered.ca:80");
        config.turnUser = "openrelayproject";
        config.turnPass = "openrelayproject";
    }
    return config;
}

void printUsage() {
    std::cout << "AetherProxy - share your terminal over WebRTC.\n"
                 "\n"
                 "Usage:\n"
                 "  aetherproxy                     Share your shell.\n"
                 "  aetherproxy -- <command>        Share one command.\n"
                 "  <cmd> | aetherproxy             Stream piped output.\n"
                 "  aetherproxy collab              Start a collaborative session.\n"
                 "  aetherproxy connect <room>      Join a remote session.\n"
                 "\n"
                 "Options:\n"
                 "  --port <n>          HTTP port. Default 8080.\n"
                 "  --stun <server>     Add a STUN server.\n"
                 "  --turn <server>     Add a TURN server.\n"
                 "  --turn-user <name>  TURN username.\n"
                 "  --turn-pass <pass>  TURN password.\n"
                 "  --no-stun           Disable STUN.\n"
                 "  --no-turn           Disable TURN.\n"
                 "  --signal <url>      Signaling server URL. Enables internet mode.\n"
                 "  --max-clients <n>   Client limit. Default 10, max 32.\n"
                 "  --idle-timeout <n>  Exit after n seconds without clients. Default 3600.\n"
                 "  -h, --help          Show this help.\n"
                 "  -V, --version       Show version.\n"
                 "\n"
                 "Config file: ~/.config/aetherproxy/config.toml\n";
}

std::string versionString() {
    return AETHER_VERSION;
}
