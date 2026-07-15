#include "client/native_client.hpp"
#include "common/raw_tty.hpp"
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <csignal>
#include <cstdlib>
#include <chrono>
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

void NativeClient::connect(const std::string &signalingUrl, const std::string &roomCode) {
    session->initialize({}, {}, "", "", false, false, false);

    std::ostringstream idStream;
    idStream << "nc-" << std::hex << reinterpret_cast<uintptr_t>(this);
    const std::string clientId = idStream.str();

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
            eq.push(Event{Event::Type::Disconnect, "", "", "", nullptr});
        }
    });

    session->onOpen([this]() { handleSigwinch(0); });

    session->onMessage([this](std::string msg) { eq.push(Event{Event::Type::Message, "", "", std::move(msg), nullptr}); });

    session->onDisconnect([this]() { eq.push(Event{Event::Type::Disconnect, "", "", "", nullptr}); });

    signalClient->onOpen([this, clientId]() {
        json helloMsg = {{"clientId", clientId}, {"name", localUserName()}};
        signalClient->send("hello", helloMsg.dump());
    });

    signalClient->connect(signalingUrl, roomCode);

    auto old = ::signal(SIGWINCH, handleSigwinch);
    (void)old;
}

void NativeClient::applyLocalWinch() {
    struct winsize ws {};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        return;
    }
    termRows = ws.ws_row;
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
    std::ostringstream line;
    line << "\0337\033[" << termRows << ";1H\033[2K\033[2m[AetherProxy] " << session->stateString();
    long rtt = session->rttMillis();
    if (rtt >= 0) {
        line << " · rtt " << rtt << " ms";
    }
    line << "\033[22m\0338";
    writeAllOut(line.str());
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
            0, 100000
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
                        } catch (...) {
                        }
                        continue;
                    }
                    writeAllOut(msg);
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
