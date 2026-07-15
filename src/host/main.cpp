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
#include "common/config.hpp"
#include "common/pty_source.hpp"
#include "common/stdin_source.hpp"
#include "common/event_queue.hpp"
#include "common/webrtc_session.hpp"
#include "common/signaling_client.hpp"
#include "common/room_code.hpp"
#include "common/qr_code.hpp"
#include "host/http_server.hpp"
#include "host/session_registry.hpp"
#include "client/native_client.hpp"

enum class SessionMode {
    Terminal,
    Command,
    Pipe,
    Client,
    Collab
};

bool hasFlag(const std::vector<std::string_view>& args, std::string_view flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

/// Returns the first non-loopback IPv4 LAN address, or "127.0.0.1" as fallback.
static std::string getLanIp() {
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return "127.0.0.1";
    std::string result = "127.0.0.1";
    for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        uint32_t addr = ntohl(sa->sin_addr.s_addr);
        if ((addr >> 24) == 127) continue; // skip loopback
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            result = buf;
            break;
        }
    }
    freeifaddrs(ifap);
    return result;
}

SessionMode detectMode(const std::vector<std::string_view>& args) {
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

int main(int argc, char* argv[]) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
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
    std::cout << "Room: " << room << std::endl;
    renderQRCode("http://" + lanIp + ":" + std::to_string(config.port) + "/#" + room);

    std::shared_ptr<IOSource> io;
    if (mode == SessionMode::Pipe) {
        io = std::make_shared<StdinSource>();
    } else if (mode == SessionMode::Command) {
        std::vector<std::string> cmd;
        bool foundDelim = false;
        for (int i = 1; i < argc; ++i) {
            if (foundDelim) {
                cmd.push_back(argv[i]);
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

    registry.onOwnerDisconnect([&running]() {
        running = false;
    });

    std::unordered_map<std::string, std::shared_ptr<WebRTCSession>> sessions;
    std::mutex sessionsMutex;

    HttpServer server;
    std::thread serverThread([&server, &config, &eq, &sessions, &sessionsMutex]() {
        std::cout << "Server listening on port " << config.port << std::endl;
        server.start(config.port,
            [&config, &eq, &sessions, &sessionsMutex](std::string clientId) {
                std::lock_guard<std::mutex> lock(sessionsMutex);
                auto it = sessions.find(clientId);
                if (it != sessions.end()) {
                    return it->second->createOffer();
                }
                auto session = std::make_shared<WebRTCSession>();
                session->initialize(config.stunServers, config.turnServers, config.turnUser, config.turnPass, config.noStun, config.noTurn);
                session->onOpen([&eq, clientId, session]() {
                    eq.push(Event{Event::Type::Join, clientId, "Collab", "", session->getChannel()});
                });
                session->onMessage([&eq, clientId](std::string msg) {
                    eq.push(Event{Event::Type::Message, clientId, "", msg, nullptr});
                });
                session->onDisconnect([&eq, clientId]() {
                    eq.push(Event{Event::Type::Disconnect, clientId, "", "", nullptr});
                });
                sessions[clientId] = session;
                return session->createOffer();
            },
            [&sessions, &sessionsMutex](std::string clientId, std::string answer) {
                std::lock_guard<std::mutex> lock(sessionsMutex);
                auto it = sessions.find(clientId);
                if (it != sessions.end()) {
                    it->second->setAnswer(answer);
                }
            }
        );
    });
    serverThread.detach();

    // Internet mode: if --signal was supplied, join the signaling room as host
    // and handle incoming peer offers exactly like the HTTP path does.
    std::shared_ptr<SignalingClient> sigClient;
    if (!config.signalingUrl.empty()) {
        sigClient = std::make_shared<SignalingClient>();
        sigClient->onMessage([&](std::string type, std::string data) {
            if (type != "offer") return;
            // Each offer encodes the clientId as part of the JSON data field.
            std::string clientId;
            std::string sdp;
            try {
                auto j = nlohmann::json::parse(data);
                clientId = j.value("clientId", "");
                sdp      = j.value("sdp", "");
            } catch (...) {
                // Legacy plain-SDP path: generate a random clientId.
                clientId = "sig-" + std::to_string(reinterpret_cast<uintptr_t>(&data));
                sdp = data;
            }
            if (clientId.empty() || sdp.empty()) return;

            std::lock_guard<std::mutex> lock(sessionsMutex);
            if (sessions.count(clientId)) return; // already connected

            auto session = std::make_shared<WebRTCSession>();
            session->initialize(config.stunServers, config.turnServers,
                                config.turnUser, config.turnPass,
                                config.noStun, config.noTurn);
            session->onOpen([&eq, clientId, session]() {
                eq.push(Event{Event::Type::Join, clientId, "Collab", "", session->getChannel()});
            });
            session->onMessage([&eq, clientId](std::string msg) {
                eq.push(Event{Event::Type::Message, clientId, "", msg, nullptr});
            });
            session->onDisconnect([&eq, clientId]() {
                eq.push(Event{Event::Type::Disconnect, clientId, "", "", nullptr});
            });
            sessions[clientId] = session;

            session->setOffer(sdp);
            std::string answer = session->createAnswer();
            nlohmann::json resp = {{"clientId", clientId}, {"sdp", answer}};
            sigClient->send("answer", resp.dump());
        });
        sigClient->connect(config.signalingUrl, room);
        std::cout << "Signaling connected: " << config.signalingUrl
                  << "  room=" << room << std::endl;
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

    auto lastInactivityCheck = std::chrono::steady_clock::now();

    while (running) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastInactivityCheck).count() >= 5) {
            registry.checkInactivity();
            lastInactivityCheck = now;
        }

        epoll_event events[4];
        int nfds = epoll_wait(epollFd, events, 4, 100);
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == eq.getFd()) {
                uint64_t val = 0;
                eventfd_read(eq.getFd(), &val);
                while (true) {
                    Event evItem = eq.pop();
                    if (evItem.type == Event::Type::None) {
                        break;
                    }
                    if (evItem.type == Event::Type::Join) {
                        Permission perm = Permission::Collaborator;
                        if (!hasOwner) {
                            perm = Permission::Owner;
                            hasOwner = true;
                        }
                        registry.addClient(evItem.clientId, "Collab", perm, evItem.channel);
                        // Announce session mode so the browser can lock input if needed
                        std::string modeStr = (mode == SessionMode::Pipe) ? "pipe" : "terminal";
                        std::string modeFrame = std::string("\x00AETHER:", 8)
                            + "{\"type\":\"mode\",\"mode\":\"" + modeStr + "\"}";
                        if (evItem.channel) {
                            evItem.channel->send(modeFrame);
                        }
                    } else if (evItem.type == Event::Type::Disconnect) {
                        registry.removeClient(evItem.clientId);
                        // Erase stale session so a reconnecting client gets a fresh PeerConnection.
                        std::lock_guard<std::mutex> lock(sessionsMutex);
                        sessions.erase(evItem.clientId);
                    } else if (evItem.type == Event::Type::Message) {
                        std::string msg = std::move(evItem.data);
                        if (msg.compare(0, 8, std::string("\x00" "AETHER:", 8)) == 0) {
                            try {
                                auto payload = nlohmann::json::parse(msg.substr(8));
                                if (payload.contains("type") && payload["type"] == "resize") {
                                    uint16_t r = payload["rows"];
                                    uint16_t c = payload["cols"];
                                    registry.updateClientSize(evItem.clientId, {r, c});
                                }
                            } catch (...) {}
                        } else {
                            registry.handleInput(evItem.clientId, msg, [&io](std::string_view d) {
                                io->write(d.data(), d.size());
                            });
                        }
                    }
                }
            } else if (events[i].data.fd == io->getFd()) {
                char buf[1024];
                ssize_t n = io->read(buf, sizeof(buf));
                if (n > 0) {
                    registry.broadcastOutput(std::string_view(buf, n));
                } else if (n == 0 && mode == SessionMode::Pipe) {
                    registry.broadcastControl(std::string("{\"type\":\"eof\"}"));
                    running = false;
                } else if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // PTY master returned EIO: child shell has exited.
                    running = false;
                }
            }
        }
    }

    close(epollFd);
    return 0;
}
