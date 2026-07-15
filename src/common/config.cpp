#include "common/config.hpp"
#include <string_view>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cctype>
#include <map>
#include <filesystem>
#include <algorithm>

#ifndef AETHER_VERSION
#define AETHER_VERSION "dev"
#endif

static const char *DEFAULT_SIGNALING_URL = "wss://signal.aetherproxy.dev";

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

static std::string tokenAfter(std::string_view text, size_t pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    size_t end = pos;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n' && text[end] != '\r') {
        ++end;
    }
    return std::string(text.substr(pos, end - pos));
}

static std::string configFilePath() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/aetherproxy/config.toml";
    }
    const char *home = std::getenv("HOME");
    if (home == nullptr) {
        return "";
    }
    return std::string(home) + "/.config/aetherproxy/config.toml";
}

static bool writeFileConfig(const std::map<std::string, std::string> &values);

static void ensureConfigFile() {
    std::string path = configFilePath();
    if (path.empty() || std::filesystem::exists(path)) {
        return;
    }
    const std::map<std::string, std::string> defaults = {
        {"port", "8080"},
        {"max-clients", "10"},
        {"idle-timeout", "3600"},
        {"stun", "stun:stun.l.google.com:19302"},
        {"turn", "turn:openrelay.metered.ca:80"},
        {"turn-user", "openrelayproject"},
        {"turn-pass", "openrelayproject"},
        {"no-stun", "false"},
        {"no-turn", "false"},
        {"offline", "false"},
        {"admit", "true"},
        {"signal", DEFAULT_SIGNALING_URL},
    };
    writeFileConfig(defaults);
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
    ensureConfigFile();
    std::ifstream file(configFilePath());
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
    size_t flagPos = findKey(ice, "no_stun");
    if (flagPos != std::string_view::npos) {
        config.noStun = tokenAfter(ice, flagPos) == "true";
    }
    flagPos = findKey(ice, "no_turn");
    if (flagPos != std::string_view::npos) {
        config.noTurn = tokenAfter(ice, flagPos) == "true";
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
    keyPos = findKey(session, "signal");
    if (keyPos != std::string_view::npos) {
        size_t p = keyPos;
        std::string v = extractQuoted(session, p);
        if (!v.empty()) {
            config.signalingUrl = v;
        }
    }
    keyPos = findKey(session, "offline");
    if (keyPos != std::string_view::npos) {
        config.offline = tokenAfter(session, keyPos) == "true";
    }
    keyPos = findKey(session, "admit");
    if (keyPos != std::string_view::npos) {
        config.admit = tokenAfter(session, keyPos) == "true";
    }
}

CLIConfig parseCLIArgs(int argc, char *argv[]) {
    CLIConfig config;
    loadConfigFile(config);

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--") {
            break;
        }
        if (arg == "--offline") {
            config.offline = true;
        } else if (arg == "--record") {
            if (i + 1 >= argc || std::string_view(argv[i + 1]) == "--") {
                std::cerr << "--record needs a file path\n";
                std::exit(1);
            }
            config.recordPath = argv[++i];
        } else if (arg.starts_with("--")) {
            std::cerr << "Unknown flag: " << arg << "\n"
                      << "Settings live in: aetherproxy config\n";
            std::exit(1);
        }
    }

    if (config.offline) {
        config.noStun = true;
        config.noTurn = true;
        config.signalingUrl.clear();
    } else if (config.signalingUrl.empty()) {
        config.signalingUrl = DEFAULT_SIGNALING_URL;
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
                 "  aetherproxy                Share your shell.\n"
                 "  aetherproxy -- <command>   Share one command.\n"
                 "  <cmd> | aetherproxy        Stream piped output.\n"
                 "\n"
                 "Options:\n"
                 "  interactive <room> Join a session in your browser.\n"
                 "  connect <room>     Join a session in this terminal.\n"
                 "  config <op>        Manage saved settings.\n"
                 "  help               Show this help.\n"
                 "  version            Show version.\n"
                 "  --offline          One run with no external servers.\n"
                 "  --record <file>    Save the session as asciicast v2.\n"
                 "\n"
                 "Config:\n"
                 "  aetherproxy config list\n"
                 "  aetherproxy config get <key>\n"
                 "  aetherproxy config set <key> <value>\n"
                 "  aetherproxy config unset <key>\n"
                 "\n"
                 "  List shows all keys and current values.\n"
                 "\n"
                 "Config file: "
              << configFilePath() << "\n";
}

std::string versionString() {
    return AETHER_VERSION;
}

static const std::map<std::string, char> CONFIG_KEYS = {
    {"port", 'n'},      {"max-clients", 'n'}, {"idle-timeout", 'n'}, {"stun", 's'},   {"turn", 's'},    {"turn-user", 's'},
    {"turn-pass", 's'}, {"no-stun", 'b'},     {"no-turn", 'b'},      {"signal", 's'}, {"offline", 'b'}, {"admit", 'b'},
};

static std::map<std::string, std::string> readFileConfig() {
    std::map<std::string, std::string> values;
    ensureConfigFile();
    std::ifstream file(configFilePath());
    if (!file) {
        return values;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();

    std::string_view ice = sectionText(content, "[ice]");
    size_t keyPos = findKey(ice, "stun");
    if (keyPos != std::string_view::npos) {
        size_t open = ice.find('[', keyPos);
        size_t close = ice.find(']', open);
        if (open != std::string_view::npos && close != std::string_view::npos) {
            std::string_view arr = ice.substr(open, close - open);
            std::string joined;
            size_t pos = 0;
            while (pos != std::string_view::npos) {
                std::string v = extractQuoted(arr, pos);
                if (!v.empty()) {
                    if (!joined.empty()) {
                        joined += ",";
                    }
                    joined += v;
                }
            }
            if (!joined.empty()) {
                values["stun"] = joined;
            }
        }
    }
    keyPos = findKey(ice, "turn");
    if (keyPos != std::string_view::npos) {
        size_t open = ice.find('{', keyPos);
        size_t close = ice.find('}', open);
        if (open != std::string_view::npos && close != std::string_view::npos) {
            std::string_view block = ice.substr(open, close - open);
            size_t p = findKey(block, "url");
            if (p != std::string_view::npos) {
                values["turn"] = extractQuoted(block, p);
            }
            p = findKey(block, "username");
            if (p != std::string_view::npos) {
                values["turn-user"] = extractQuoted(block, p);
            }
            p = findKey(block, "credential");
            if (p != std::string_view::npos) {
                values["turn-pass"] = extractQuoted(block, p);
            }
        }
    }
    keyPos = findKey(ice, "no_stun");
    if (keyPos != std::string_view::npos) {
        values["no-stun"] = tokenAfter(ice, keyPos);
    }
    keyPos = findKey(ice, "no_turn");
    if (keyPos != std::string_view::npos) {
        values["no-turn"] = tokenAfter(ice, keyPos);
    }

    std::string_view session = sectionText(content, "[session]");
    for (const auto &[fileKey, flatKey] : std::vector<std::pair<std::string, std::string>>{
             {"port", "port"}, {"max_clients", "max-clients"}, {"idle_timeout", "idle-timeout"}}) {
        size_t p = findKey(session, fileKey);
        if (p != std::string_view::npos) {
            long v = parseNumberAt(session, p);
            if (v >= 0) {
                values[flatKey] = std::to_string(v);
            }
        }
    }
    keyPos = findKey(session, "signal");
    if (keyPos != std::string_view::npos) {
        size_t p = keyPos;
        std::string v = extractQuoted(session, p);
        if (!v.empty()) {
            values["signal"] = v;
        }
    }
    keyPos = findKey(session, "offline");
    if (keyPos != std::string_view::npos) {
        values["offline"] = tokenAfter(session, keyPos);
    }
    keyPos = findKey(session, "admit");
    if (keyPos != std::string_view::npos) {
        values["admit"] = tokenAfter(session, keyPos);
    }
    return values;
}

static bool writeFileConfig(const std::map<std::string, std::string> &values) {
    std::string path = configFilePath();
    if (path.empty()) {
        return false;
    }
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }

    bool hasIce = values.contains("stun") || values.contains("turn") || values.contains("turn-user") || values.contains("turn-pass") ||
                  values.contains("no-stun") || values.contains("no-turn");
    if (hasIce) {
        out << "[ice]\n";
        if (values.contains("stun")) {
            out << "stun = [";
            std::string_view rest = values.at("stun");
            bool first = true;
            while (!rest.empty()) {
                size_t comma = rest.find(',');
                std::string_view item = rest.substr(0, comma);
                if (!item.empty()) {
                    out << (first ? "" : ", ") << '"' << item << '"';
                    first = false;
                }
                rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
            }
            out << "]\n";
        }
        if (values.contains("turn") || values.contains("turn-user") || values.contains("turn-pass")) {
            out << "turn = [ { url = \"" << (values.contains("turn") ? values.at("turn") : "") << "\", username = \""
                << (values.contains("turn-user") ? values.at("turn-user") : "") << "\", credential = \""
                << (values.contains("turn-pass") ? values.at("turn-pass") : "") << "\" } ]\n";
        }
        if (values.contains("no-stun")) {
            out << "no_stun = " << values.at("no-stun") << "\n";
        }
        if (values.contains("no-turn")) {
            out << "no_turn = " << values.at("no-turn") << "\n";
        }
        out << "\n";
    }

    bool hasSession = values.contains("port") || values.contains("max-clients") || values.contains("idle-timeout") ||
                      values.contains("signal") || values.contains("offline") || values.contains("admit");
    if (hasSession) {
        out << "[session]\n";
        if (values.contains("port")) {
            out << "port = " << values.at("port") << "\n";
        }
        if (values.contains("max-clients")) {
            out << "max_clients = " << values.at("max-clients") << "\n";
        }
        if (values.contains("idle-timeout")) {
            out << "idle_timeout = " << values.at("idle-timeout") << "\n";
        }
        if (values.contains("signal")) {
            out << "signal = \"" << values.at("signal") << "\"\n";
        }
        if (values.contains("offline")) {
            out << "offline = " << values.at("offline") << "\n";
        }
        if (values.contains("admit")) {
            out << "admit = " << values.at("admit") << "\n";
        }
    }
    return true;
}

static bool validConfigValue(char kind, const std::string &value) {
    if (kind == 'n') {
        return !value.empty() && std::ranges::all_of(value, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    }
    if (kind == 'b') {
        return value == "true" || value == "false";
    }
    return !value.empty();
}

int runConfigCommand(const std::vector<std::string> &args) {
    auto keysLine = []() {
        std::string keys;
        for (const auto &[key, kind] : CONFIG_KEYS) {
            if (!keys.empty()) {
                keys += ", ";
            }
            keys += key;
        }
        return keys;
    };

    if (args.empty() || args[0] == "list") {
        for (const auto &[key, value] : readFileConfig()) {
            std::cout << key << "=" << value << "\n";
        }
        return 0;
    }
    const std::string &op = args[0];
    if (op == "get" && args.size() == 2) {
        auto values = readFileConfig();
        auto it = values.find(args[1]);
        if (it == values.end()) {
            return 1;
        }
        std::cout << it->second << "\n";
        return 0;
    }
    if (op == "set" && args.size() == 3) {
        auto keyIt = CONFIG_KEYS.find(args[1]);
        if (keyIt == CONFIG_KEYS.end()) {
            std::cerr << "Unknown key: " << args[1] << "\nKeys: " << keysLine() << "\n";
            return 1;
        }
        if (!validConfigValue(keyIt->second, args[2])) {
            std::cerr << "Bad value for " << args[1] << ": " << args[2] << "\n";
            return 1;
        }
        auto values = readFileConfig();
        values[args[1]] = args[2];
        if (!writeFileConfig(values)) {
            std::cerr << "Cannot write config file.\n";
            return 1;
        }
        return 0;
    }
    if (op == "unset" && args.size() == 2) {
        auto values = readFileConfig();
        values.erase(args[1]);
        if (!writeFileConfig(values)) {
            std::cerr << "Cannot write config file.\n";
            return 1;
        }
        return 0;
    }
    std::cerr << "Usage:\n"
                 "  aetherproxy config list\n"
                 "  aetherproxy config get <key>\n"
                 "  aetherproxy config set <key> <value>\n"
                 "  aetherproxy config unset <key>\n"
                 "Keys: "
              << keysLine() << "\n";
    return 1;
}
