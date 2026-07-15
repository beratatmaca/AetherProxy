#include <iostream>
#include <unistd.h>
#include <string_view>
#include <algorithm>
#include <vector>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <csignal>
#include "common/raw_tty.hpp"
#include "common/config.hpp"
#include "common/pty_source.hpp"
#include "common/stdin_source.hpp"
#include "common/event_queue.hpp"
#include "common/webrtc_session.hpp"
#include "common/signaling_client.hpp"
#include "common/room_code.hpp"
#include "common/banner.hpp"
#include "host/http_server.hpp"
#include "host/session_registry.hpp"
#include "client/native_client.hpp"

enum class SessionMode : std::uint8_t { Terminal, Command, Pipe, Client, Collab };

bool hasFlag(const std::vector<std::string_view> &args, std::string_view flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

static bool isVirtualInterface(const char *name) {
    static const std::vector<std::string_view> prefixes = {"docker", "br-", "veth", "virbr", "vmnet", "tun", "tap", "wg"};
    std::string_view n(name);
    return std::ranges::any_of(prefixes, [&](std::string_view p) { return n.substr(0, p.size()) == p; });
}

static std::string getLanIp() {
    struct ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0)
        return "127.0.0.1";
    std::string result = "127.0.0.1";
    for (struct ifaddrs *ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (ifa->ifa_name && isVirtualInterface(ifa->ifa_name))
            continue;
        auto *sa = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
        uint32_t addr = ntohl(sa->sin_addr.s_addr);
        if ((addr >> 24) == 127)
            continue;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            result = buf;
            break;
        }
    }
    freeifaddrs(ifap);
    return result;
}

static int host_winch_fd = -1;

static void handleHostSigwinch(int) {
    if (host_winch_fd != -1) {
        uint64_t val = 1;
        ssize_t r = write(host_winch_fd, &val, sizeof(val));
        (void)r;
    }
}

static void writeFull(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

SessionMode detectMode(const std::vector<std::string_view> &args) {
    if (isatty(STDIN_FILENO) == 0) {
        return SessionMode::Pipe;
    }
    if (hasFlag(args, "connect")) {
        return SessionMode::Client;
    }
    if (hasFlag(args, "collab")) {
        return SessionMode::Collab;
    }
    if (hasFlag(args, "--")) {
        return SessionMode::Command;
    }
    return SessionMode::Terminal;
}

int main(int argc, char *argv[]) {
    std::vector<std::string_view> args(argv + 1, argv + argc);

    auto delim = std::find(args.begin(), args.end(), "--");
    std::vector<std::string_view> ownArgs(args.begin(), delim);
    if (hasFlag(ownArgs, "--help") || hasFlag(ownArgs, "-h")) {
        printUsage();
        return 0;
    }
    if (hasFlag(ownArgs, "--version") || hasFlag(ownArgs, "-V")) {
        std::cout << "aetherproxy " << versionString() << "\n" << std::flush;
        return 0;
    }

    SessionMode mode = detectMode(args);
    CLIConfig config = parseCLIArgs(argc, argv);

    if (mode == SessionMode::Client) {
        if (argc < 3) {
            std::cerr << "Usage: aetherproxy connect <room> [--signal <url>]\n";
            return 1;
        }
        if (config.signalingUrl.empty()) {
            std::cerr << "Error: internet mode requires --signal <ws://server>\n";
            return 1;
        }
        NativeClient client;
        client.connect(config.signalingUrl, argv[2]);
        client.run();
        return 0;
    }

    std::string room = generateRoomCode();
    std::string lanIp = getLanIp();
    std::string url = "http://" + lanIp + ":" + std::to_string(config.port) + "/#" + room;
    renderStartupScreen(room, url);

    std::shared_ptr<IOSource> io;
    if (mode == SessionMode::Pipe) {
        io = std::make_shared<StdinSource>();
    } else if (mode == SessionMode::Command) {
        std::vector<std::string> cmd;
        bool foundDelim = false;
        for (int i = 1; i < argc; ++i) {
            if (foundDelim) {
                cmd.emplace_back(argv[i]);
            } else if (std::string_view(argv[i]) == "--") {
                foundDelim = true;
            }
        }
        io = std::make_shared<PTYSource>(cmd);
    } else {
        io = std::make_shared<PTYSource>();
    }

    EventQueue eq;
    int ptyFd = -1;
    if (mode != SessionMode::Pipe) {
        ptyFd = std::static_pointer_cast<PTYSource>(io)->getFd();
    }
    SessionRegistry registry(ptyFd, config.maxClients);

    std::atomic<bool> running{true};
    bool hasOwner = false;

    registry.onOwnerDisconnect([&running]() { running = false; });

    std::unordered_map<std::string, std::shared_ptr<WebRTCSession>> sessions;
    std::mutex sessionsMutex;
    const size_t sessionLimit = static_cast<size_t>(std::min(config.maxClients, 32));

    HttpServer server;
    std::thread serverThread([&server, &config, &eq, &sessions, &sessionsMutex, sessionLimit]() {
        std::cout << "Server listening on port " << config.port << "\n" << std::flush;
        server.start(
            config.port,
            [&config, &eq, &sessions, &sessionsMutex, sessionLimit](const std::string &clientId) {
                std::lock_guard<std::mutex> lock(sessionsMutex);
                auto it = sessions.find(clientId);
                if (it != sessions.end()) {
                    return it->second->createOffer();
                }
                if (sessions.size() >= sessionLimit) {
                    return std::string();
                }
                auto session = std::make_shared<WebRTCSession>();
                session->initialize(config.stunServers, config.turnServers, config.turnUser, config.turnPass, config.noStun, config.noTurn);
                session->onOpen(
                    [&eq, clientId, session]() { eq.push(Event{Event::Type::Join, clientId, "Collab", "", session->getChannel()}); });
                session->onMessage(
                    [&eq, clientId](std::string msg) { eq.push(Event{Event::Type::Message, clientId, "", std::move(msg), nullptr}); });
                session->onDisconnect([&eq, clientId]() { eq.push(Event{Event::Type::Disconnect, clientId, "", "", nullptr}); });
                sessions[clientId] = session;
                return session->createOffer();
            },
            [&sessions, &sessionsMutex](const std::string &clientId, const std::string &answer) {
                std::lock_guard<std::mutex> lock(sessionsMutex);
                auto it = sessions.find(clientId);
                if (it != sessions.end()) {
                    it->second->setAnswer(answer);
                }
            });
    });
    serverThread.detach();

    std::shared_ptr<SignalingClient> sigClient;
    if (!config.signalingUrl.empty()) {
        sigClient = std::make_shared<SignalingClient>();
        sigClient->onMessage([&](const std::string &type, const std::string &data) {
            if (type == "hello") {
                std::string clientId;
                try {
                    auto j = nlohmann::json::parse(data);
                    clientId = j.value("clientId", "");
                } catch (...) {
                    return;
                }
                if (clientId.empty())
                    return;

                std::lock_guard<std::mutex> lock(sessionsMutex);
                if (sessions.contains(clientId))
                    return;
                if (sessions.size() >= sessionLimit) {
                    nlohmann::json err = {{"clientId", clientId}, {"message", "session full"}};
                    sigClient->send("error", err.dump());
                    return;
                }

                auto session = std::make_shared<WebRTCSession>();
                session->initialize(config.stunServers, config.turnServers, config.turnUser, config.turnPass, config.noStun, config.noTurn);
                session->onOpen(
                    [&eq, clientId, session]() { eq.push(Event{Event::Type::Join, clientId, "Collab", "", session->getChannel()}); });
                session->onMessage(
                    [&eq, clientId](std::string msg) { eq.push(Event{Event::Type::Message, clientId, "", std::move(msg), nullptr}); });
                session->onDisconnect([&eq, clientId]() { eq.push(Event{Event::Type::Disconnect, clientId, "", "", nullptr}); });
                sessions[clientId] = session;

                std::string offer = session->createOffer();
                nlohmann::json resp = {{"clientId", clientId}, {"sdp", offer}};
                sigClient->send("offer", resp.dump());
            } else if (type == "answer") {
                std::string clientId;
                std::string sdp;
                try {
                    auto j = nlohmann::json::parse(data);
                    clientId = j.value("clientId", "");
                    sdp = j.value("sdp", "");
                } catch (...) {
                    return;
                }
                if (clientId.empty() || sdp.empty())
                    return;

                std::lock_guard<std::mutex> lock(sessionsMutex);
                auto it = sessions.find(clientId);
                if (it != sessions.end()) {
                    it->second->setAnswer(sdp);
                }
            }
        });
        sigClient->connect(config.signalingUrl, room);
        std::cout << "Signaling connected: " << config.signalingUrl << "  room=" << room << "\n" << std::flush;
    }

    int epollFd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = eq.getFd();
    epoll_ctl(epollFd, EPOLL_CTL_ADD, eq.getFd(), &ev);

    epoll_event ioEv{};
    ioEv.events = EPOLLIN;
    ioEv.data.fd = io->getFd();
    epoll_ctl(epollFd, EPOLL_CTL_ADD, io->getFd(), &ioEv);

    bool localTty = mode != SessionMode::Pipe && isatty(STDIN_FILENO) != 0;
    int winchFd = -1;
    if (localTty) {
        struct winsize hostWs{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &hostWs) == 0) {
            registry.setBaseSize({hostWs.ws_row, hostWs.ws_col});
        }
        winchFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        host_winch_fd = winchFd;
        ::signal(SIGWINCH, handleHostSigwinch);

        epoll_event stdinEv{};
        stdinEv.events = EPOLLIN;
        stdinEv.data.fd = STDIN_FILENO;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, STDIN_FILENO, &stdinEv);

        epoll_event winchEv{};
        winchEv.events = EPOLLIN;
        winchEv.data.fd = winchFd;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, winchFd, &winchEv);

        enableRawTty();
    }

    auto lastInactivityCheck = std::chrono::steady_clock::now();

    std::string pipeBacklog;
    bool eofSeen = false;
    constexpr size_t kBacklogMax = 1U << 20U;
    constexpr size_t kReplayChunk = 16384;

    auto lastClientSeen = std::chrono::steady_clock::now();

    while (running) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastInactivityCheck).count() >= 5) {
            registry.checkInactivity();
            lastInactivityCheck = now;
        }

        if (registry.clientCount() > 0) {
            lastClientSeen = now;
        } else if (!localTty && config.idleTimeout > 0 &&
                   std::chrono::duration_cast<std::chrono::seconds>(now - lastClientSeen).count() >= config.idleTimeout) {
            running = false;
        }

        epoll_event events[8];
        int nfds = epoll_wait(epollFd, events, 8, 100);
        for (int i = 0; i < nfds; ++i) {
            if (localTty && events[i].data.fd == STDIN_FILENO) {
                char buf[1024];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    io->write(buf, static_cast<size_t>(n));
                } else if (n == 0) {
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
                }
            } else if (localTty && events[i].data.fd == winchFd) {
                uint64_t val = 0;
                ssize_t r = read(winchFd, &val, sizeof(val));
                (void)r;
                struct winsize hostWs{};
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &hostWs) == 0) {
                    registry.setBaseSize({hostWs.ws_row, hostWs.ws_col});
                }
            } else if (events[i].data.fd == eq.getFd()) {
                uint64_t val = 0;
                eventfd_read(eq.getFd(), &val);
                while (true) {
                    Event evItem = eq.pop();
                    if (evItem.type == Event::Type::None) {
                        break;
                    }
                    if (evItem.type == Event::Type::Join) {
                        Permission perm = Permission::Collaborator;
                        if (mode == SessionMode::Collab && !hasOwner) {
                            perm = Permission::Owner;
                            hasOwner = true;
                        }
                        registry.addClient(evItem.clientId, "Collab", perm, evItem.channel);
                        std::string modeStr = "terminal";
                        if (mode == SessionMode::Pipe) {
                            modeStr = "pipe";
                        } else if (mode == SessionMode::Collab) {
                            modeStr = "collab";
                        }
                        std::string modeFrame = std::string(
                                                    "\x00"
                                                    "AETHER:",
                                                    8) +
                                                R"({"type":"mode","mode":")" + modeStr + R"("})";
                        if (evItem.channel) {
                            evItem.channel->send(modeFrame);
                            for (size_t off = 0; off < pipeBacklog.size(); off += kReplayChunk) {
                                evItem.channel->send(pipeBacklog.substr(off, kReplayChunk));
                            }
                            if (eofSeen) {
                                evItem.channel->send(std::string("\x00"
                                                                 "AETHER:",
                                                                 8) +
                                                     R"({"type":"eof"})");
                            }
                        }
                    } else if (evItem.type == Event::Type::Disconnect) {
                        registry.removeClient(evItem.clientId);
                        std::lock_guard<std::mutex> lock(sessionsMutex);
                        sessions.erase(evItem.clientId);
                    } else if (evItem.type == Event::Type::Message) {
                        std::string msg = std::move(evItem.data);
                        if (msg.compare(0, 8,
                                        std::string("\x00"
                                                    "AETHER:",
                                                    8)) == 0) {
                            try {
                                auto payload = nlohmann::json::parse(msg.substr(8));
                                if (payload.contains("type") && payload["type"] == "resize") {
                                    uint16_t r = payload["rows"];
                                    uint16_t c = payload["cols"];
                                    registry.updateClientSize(evItem.clientId, {r, c});
                                }
                            } catch (...) {
                                continue;
                            }
                        } else {
                            registry.handleInput(evItem.clientId, msg, [&io](std::string_view d) { io->write(d.data(), d.size()); });
                        }
                    }
                }
            } else if (events[i].data.fd == io->getFd()) {
                char buf[1024];
                ssize_t n = io->read(buf, sizeof(buf));
                if (n > 0) {
                    registry.broadcastOutput(std::string_view(buf, n));
                    if (localTty) {
                        writeFull(STDOUT_FILENO, buf, static_cast<size_t>(n));
                    }
                    if (mode == SessionMode::Pipe) {
                        pipeBacklog.append(buf, static_cast<size_t>(n));
                        if (pipeBacklog.size() > kBacklogMax) {
                            pipeBacklog.erase(0, pipeBacklog.size() - kBacklogMax);
                        }
                    }
                } else if (n == 0 && mode == SessionMode::Pipe) {
                    registry.broadcastControl(std::string(R"({"type":"eof"})"));
                    eofSeen = true;
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, io->getFd(), nullptr);
                } else if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    running = false;
                }
            }
        }
    }

    if (localTty) {
        disableRawTty();
        if (winchFd != -1) {
            close(winchFd);
        }
        std::cout << "\nSession ended.\n" << std::flush;
    }
    close(epollFd);
    return 0;
}
