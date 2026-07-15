#include "client/native_client.hpp"
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static struct termios orig_termios;
static bool raw_mode_active = false;
static int global_winch_fd = -1;

void disableRawMode() {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = false;
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return;
    struct termios raw = orig_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != -1) {
        raw_mode_active = true;
        std::atexit(disableRawMode);
    }
}

void handleSigwinch(int) {
    if (global_winch_fd != -1) {
        uint64_t val = 1;
        write(global_winch_fd, &val, sizeof(val));
    }
}

NativeClient::NativeClient() : signalClient(std::make_shared<SignalingClient>()), session(std::make_shared<WebRTCSession>()), winchFd(-1), running(false) {
    winchFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    global_winch_fd = winchFd;
}

NativeClient::~NativeClient() {
    disableRawMode();
    if (winchFd != -1) {
        close(winchFd);
    }
}

void NativeClient::connect(const std::string& signalingUrl, const std::string& roomCode) {
    session->initialize({}, {}, "", "", false, false);

    // Generate a unique client identifier for this session.
    std::ostringstream idStream;
    idStream << "nc-" << std::hex << reinterpret_cast<uintptr_t>(this);
    const std::string clientId = idStream.str();

    signalClient->onMessage([this, clientId](std::string type, std::string data) {
        if (type == "answer") {
            // Host wraps answer as {"clientId":...,"sdp":...}; fall back to plain SDP.
            std::string sdp;
            try {
                auto j = nlohmann::json::parse(data);
                sdp = j.value("sdp", "");
            } catch (...) {
                sdp = data;
            }
            if (!sdp.empty()) {
                session->setAnswer(sdp);
            }
        }
    });

    session->onCandidate([this](std::string sdp, std::string mid) {
        signalClient->send("candidate", sdp);
    });

    session->onOpen([this]() {
        handleSigwinch(0);
    });

    session->onMessage([this](std::string msg) {
        eq.push(Event{Event::Type::Message, "", "", msg, nullptr});
    });

    session->onDisconnect([this]() {
        eq.push(Event{Event::Type::Disconnect, "", "", "", nullptr});
    });

    // Connect to signaling server, then create and send our offer.
    signalClient->connect(signalingUrl, roomCode);

    std::string offerSdp = session->createOffer();
    nlohmann::json offerMsg = {{"clientId", clientId}, {"sdp", offerSdp}};
    signalClient->send("offer", offerMsg.dump());

    auto old = ::signal(SIGWINCH, handleSigwinch);
    (void)old;
}

void NativeClient::run() {
    enableRawMode();
    running = true;

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(eq.getFd(), &fds);
        FD_SET(winchFd, &fds);

        int maxFd = std::max(STDIN_FILENO, std::max(eq.getFd(), winchFd));
        struct timeval tv{0, 100000};
        int ret = select(maxFd + 1, &fds, nullptr, nullptr, &tv);

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
                read(winchFd, &val, sizeof(val));
                struct winsize ws{};
                if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
                    json msg = {
                        {"type", "resize"},
                        {"rows", ws.ws_row},
                        {"cols", ws.ws_col}
                    };
                    session->send(std::string("\x00" "AETHER:", 8) + msg.dump());
                }
            }

            if (FD_ISSET(eq.getFd(), &fds)) {
                uint64_t val = 0;
                eventfd_read(eq.getFd(), &val);
                while (true) {
                    Event ev = eq.pop();
                    if (ev.type == Event::Type::None) {
                        break;  // Queue exhausted — stop draining.
                    }
                    if (ev.type == Event::Type::Disconnect) {
                        running = false;
                        break;
                    }
                    std::string msg = std::move(ev.data);
                    if (msg.compare(0, 8, std::string("\x00" "AETHER:", 8)) == 0) {
                        continue;
                    }
                    write(STDOUT_FILENO, msg.data(), msg.size());
                }
            }
        }
    }
    disableRawMode();
}
