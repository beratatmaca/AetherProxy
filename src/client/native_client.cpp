#include "client/native_client.hpp"
#include "common/raw_tty.hpp"
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <random>
#include <iostream>
#include <sstream>
#include <string_view>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static void writeAllOut(std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t w = write(STDOUT_FILENO, data.data() + sent, data.size() - sent);
        if (w <= 0) {
            return;
        }
        sent += static_cast<size_t>(w);
    }
}

static std::string base64Decode(std::string_view in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::string out;
    unsigned int buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        int v = val(c);
        if (v < 0) {
            continue;
        }
        buf = (buf << 6U) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> static_cast<unsigned int>(bits)) & 0xFFU));
        }
    }
    return out;
}

static void copyToClipboard(const std::string &text) {
    const char *cmd = nullptr;
    if (std::getenv("WAYLAND_DISPLAY") != nullptr) {
        cmd = "wl-copy 2>/dev/null";
    } else if (std::getenv("DISPLAY") != nullptr) {
        cmd = "xclip -selection clipboard -in 2>/dev/null";
    }
    if (cmd == nullptr) {
        return;
    }
    FILE *pipe = popen(cmd, "w");
    if (pipe == nullptr) {
        return;
    }
    fwrite(text.data(), 1, text.size(), pipe);
    pclose(pipe);
}

static void handleOsc52(std::string_view seq) {
    size_t payloadStart = seq.find(';', 5);
    if (payloadStart == std::string_view::npos) {
        return;
    }
    ++payloadStart;
    size_t payloadEnd = seq.size();
    if (seq.ends_with('\x07')) {
        payloadEnd -= 1;
    } else if (seq.ends_with("\033\\")) {
        payloadEnd -= 2;
    }
    if (payloadEnd <= payloadStart) {
        return;
    }
    std::string_view payload = seq.substr(payloadStart, payloadEnd - payloadStart);
    if (payload == "?") {
        return;
    }
    constexpr size_t kMaxClipB64 = (1U << 16U) * 4U / 3U;
    if (payload.size() > kMaxClipB64) {
        return;
    }
    std::string decoded = base64Decode(payload);
    if (!decoded.empty()) {
        copyToClipboard(decoded);
    }
}

static size_t partialOscPrefixLen(std::string_view buf) {
    static constexpr std::string_view kPrefix = "\033]52;";
    size_t maxLen = std::min(buf.size(), kPrefix.size() - 1);
    for (size_t len = maxLen; len > 0; --len) {
        if (buf.substr(buf.size() - len) == kPrefix.substr(0, len)) {
            return len;
        }
    }
    return 0;
}

static int global_winch_fd = -1;

void handleSigwinch(int) {
    if (global_winch_fd != -1) {
        uint64_t val = 1;
        ssize_t r = write(global_winch_fd, &val, sizeof(val));
        (void)r;
    }
}

NativeClient::NativeClient() : signalClient(std::make_shared<SignalingClient>()), session(std::make_shared<WebRTCSession>()) {
    winchFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    global_winch_fd = winchFd;
}

NativeClient::~NativeClient() {
    disableRawTty();
    if (winchFd != -1) {
        close(winchFd);
    }
}

static std::string localUserName() {
    const char *user = std::getenv("USER");
    if (user == nullptr || *user == '\0') {
        user = std::getenv("LOGNAME");
    }
    return (user != nullptr && *user != '\0') ? user : "guest";
}

void NativeClient::connect(const CLIConfig &config, const std::string &roomCode) {
    session->initialize(config.stunServers, config.turnServers, config.turnUser, config.turnPass, config.noStun, config.noTurn, false);

    std::random_device rd;
    std::ostringstream idStream;
    idStream << "nc-" << std::hex << rd() << rd();
    const std::string clientId = idStream.str();
    myId = clientId;

    signalClient->onMessage([this, clientId](const std::string &type, const std::string &data) {
        if (type == "offer") {
            std::string target;
            std::string sdp;
            try {
                auto j = nlohmann::json::parse(data);
                target = j.value("clientId", "");
                sdp = j.value("sdp", "");
            } catch (...) {
                return;
            }
            if (target != clientId || sdp.empty()) {
                return;
            }
            session->setOffer(sdp);
            std::string answer = session->createAnswer();
            json resp = {{"clientId", clientId}, {"sdp", answer}};
            signalClient->send("answer", resp.dump());
        } else if (type == "error") {
            std::string target;
            std::string message;
            try {
                auto j = nlohmann::json::parse(data);
                target = j.value("clientId", "");
                message = j.value("message", "");
            } catch (...) {
                return;
            }
            if (!target.empty() && target != clientId) {
                return;
            }
            std::cerr << "Connection rejected: " << message << "\n" << std::flush;
            eq.push(Event{.type = Event::Type::Disconnect, .clientId = "", .clientName = "", .data = "", .channel = nullptr});
        }
    });

    session->onOpen([]() { handleSigwinch(0); });

    session->onMessage([this](std::string msg) {
        eq.push(Event{.type = Event::Type::Message, .clientId = "", .clientName = "", .data = std::move(msg), .channel = nullptr});
    });

    session->onDisconnect(
        [this]() { eq.push(Event{.type = Event::Type::Disconnect, .clientId = "", .clientName = "", .data = "", .channel = nullptr}); });

    signalClient->onOpen([this, clientId]() {
        json helloMsg = {{"clientId", clientId}, {"name", localUserName()}};
        signalClient->send("hello", helloMsg.dump());
    });

    signalClient->connect(config.signalingUrl, roomCode);

    auto old = ::signal(SIGWINCH, handleSigwinch);
    (void)old;
}

void NativeClient::applyLocalWinch() {
    struct winsize ws {};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        return;
    }
    termRows = ws.ws_row;
    termCols = ws.ws_col;
    uint16_t remoteRows = ws.ws_row;
    if (termRows >= 2) {
        std::ostringstream region;
        region << "\033[1;" << (termRows - 1) << "r";
        writeAllOut(region.str());
        remoteRows = termRows - 1;
    }
    json msg = {{"type", "resize"}, {"rows", remoteRows}, {"cols", ws.ws_col}};
    session->send(std::string("\x00"
                              "AETHER:",
                              8) +
                  msg.dump());
}

void NativeClient::renderTelemetry() {
    if (termRows < 2) {
        return;
    }
    std::ostringstream text;
    text << "[AetherProxy] " << session->stateString();
    long rtt = session->rttMillis();
    if (rtt >= 0) {
        text << " · rtt " << rtt << " ms";
    }
    if (!peers.empty()) {
        text << " · " << peers;
    }
    if (pendingApproval) {
        text << " · awaiting approval";
    }
    if (!lockedBy.empty()) {
        auto held = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lockedAt).count();
        if (held < 1500) {
            text << " · ✋ " << lockedBy;
        } else {
            lockedBy.clear();
        }
    }
    std::string t = text.str();
    if (termCols > 1 && t.size() > static_cast<size_t>(termCols) - 1) {
        size_t cut = static_cast<size_t>(termCols) - 1;
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        t.resize(cut);
    }
    std::ostringstream line;
    line << "\0337\033[" << termRows << ";1H\033[2K\033[2m" << t << "\033[22m\0338";
    writeAllOut(line.str());
}

void NativeClient::processOutput(std::string_view data) {
    constexpr size_t kOscMax = 1U << 17U;
    outBuf.append(data);
    std::string flush;
    while (!outBuf.empty()) {
        if (!inOsc) {
            size_t esc = outBuf.find("\033]52;");
            if (esc == std::string::npos) {
                size_t keep = partialOscPrefixLen(outBuf);
                flush += outBuf.substr(0, outBuf.size() - keep);
                outBuf.erase(0, outBuf.size() - keep);
                break;
            }
            flush += outBuf.substr(0, esc);
            outBuf.erase(0, esc);
            inOsc = true;
        } else {
            size_t bel = outBuf.find('\x07');
            size_t st = outBuf.find("\033\\", 1);
            size_t seqEnd = std::string::npos;
            if (bel != std::string::npos && (st == std::string::npos || bel < st)) {
                seqEnd = bel + 1;
            } else if (st != std::string::npos) {
                seqEnd = st + 2;
            }
            if (seqEnd == std::string::npos) {
                if (outBuf.size() > kOscMax) {
                    flush += outBuf;
                    outBuf.clear();
                    inOsc = false;
                }
                break;
            }
            handleOsc52(std::string_view(outBuf).substr(0, seqEnd));
            outBuf.erase(0, seqEnd);
            inOsc = false;
        }
    }
    if (!flush.empty()) {
        writeAllOut(flush);
    }
}

void NativeClient::run() {
    enableRawTty();
    running = true;
    auto lastTelemetry = std::chrono::steady_clock::now();

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(eq.getFd(), &fds);
        FD_SET(winchFd, &fds);

        int maxFd = std::max(STDIN_FILENO, std::max(eq.getFd(), winchFd));
        struct timeval tv {
            .tv_sec = 0, .tv_usec = 100000
        };
        int ret = select(maxFd + 1, &fds, nullptr, nullptr, &tv);

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTelemetry).count() >= 2000) {
            renderTelemetry();
            lastTelemetry = now;
        }

        if (ret > 0) {
            if (FD_ISSET(STDIN_FILENO, &fds)) {
                char buf[1024];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    session->send(std::string(buf, n));
                } else if (n == 0) {
                    break;
                }
            }

            if (FD_ISSET(winchFd, &fds)) {
                uint64_t val = 0;
                ssize_t r = read(winchFd, &val, sizeof(val));
                (void)r;
                applyLocalWinch();
            }

            if (FD_ISSET(eq.getFd(), &fds)) {
                uint64_t val = 0;
                eventfd_read(eq.getFd(), &val);
                while (true) {
                    Event ev = eq.pop();
                    if (ev.type == Event::Type::None) {
                        break;
                    }
                    if (ev.type == Event::Type::Disconnect) {
                        running = false;
                        break;
                    }
                    std::string msg = std::move(ev.data);
                    if (msg.compare(0, 8,
                                    std::string("\x00"
                                                "AETHER:",
                                                8)) == 0) {
                        try {
                            auto payload = json::parse(msg.substr(8));
                            std::string type = payload.value("type", "");
                            if (type == "server_exit" || type == "end") {
                                writeAllOut("\r\n[AetherProxy: Server terminated session]\r\n");
                                running = false;
                                break;
                            }
                            if (type == "locked") {
                                lockedBy = payload.value("by", "");
                                lockedAt = std::chrono::steady_clock::now();
                                renderTelemetry();
                            }
                            if (type == "error") {
                                writeAllOut("\r\n[AetherProxy: " + payload.value("message", std::string("rejected")) + "]\r\n");
                                running = false;
                                break;
                            }
                            if (type == "presence") {
                                std::string line;
                                bool pending = false;
                                for (const auto &c : payload.value("clients", json::array())) {
                                    if (!c.is_object()) {
                                        continue;
                                    }
                                    if (c.value("id", "") == myId) {
                                        pending = c.value("permission", "") == "pending";
                                    }
                                    if (!line.empty()) {
                                        line += " ";
                                    }
                                    line += c.value("displayName", "");
                                    if (c.value("active", false)) {
                                        line += "*";
                                    }
                                }
                                peers = line;
                                pendingApproval = pending;
                                renderTelemetry();
                            }
                        } catch (...) {
                            continue;
                        }
                        continue;
                    }
                    processOutput(msg);
                }
            }
        }
    }
    if (termRows >= 2) {
        std::ostringstream reset;
        reset << "\033[r\033[" << termRows << ";1H\033[2K";
        writeAllOut(reset.str());
    }
    disableRawTty();
}
