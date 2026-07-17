#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <string_view>
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
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <cstdlib>
#include "common/raw_tty.hpp"
#include "common/config.hpp"
#include "common/pty_source.hpp"
#include "common/stdin_source.hpp"
#include "common/event_queue.hpp"
#include "common/logger.hpp"
#include "common/webrtc_session.hpp"
#include "common/signaling_client.hpp"
#include "common/room_code.hpp"
#include "common/banner.hpp"
#include "host/http_server.hpp"
#include "host/session_registry.hpp"
#include "host/replay_buffer.hpp"
#include "host/session_recorder.hpp"
#include <deque>
#include "client/native_client.hpp"

enum class SessionMode : std::uint8_t { Terminal, Command, Pipe, Client, Interactive };

bool hasFlag(const std::vector<std::string_view> &args, std::string_view flag) {
    return std::ranges::find(args, flag) != args.end();
}

static bool isVirtualInterface(const char *name) {
    static const std::vector<std::string_view> prefixes = {"docker", "br-", "veth", "virbr", "vmnet", "tun", "tap", "wg"};
    std::string_view n(name);
    return std::ranges::any_of(prefixes, [&](std::string_view p) { return n.starts_with(p); });
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

static volatile std::sig_atomic_t g_exitSignal = 0;

static void handleExitSignal(int) {
    g_exitSignal = 1;
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

static void launchBrowser(const std::string &url) {
    const char *env = std::getenv("BROWSER");
    std::string cmd = (env != nullptr && *env != '\0') ? env : "xdg-open";

    pid_t pid = fork();
    if (pid != 0) {
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
        return;
    }
    if (fork() != 0) {
        _exit(0);
    }
    int devNull = open("/dev/null", O_RDWR);
    if (devNull != -1) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) {
            close(devNull);
        }
    }
    execlp(cmd.c_str(), cmd.c_str(), url.c_str(), static_cast<char *>(nullptr));
    _exit(127);
}

SessionMode detectMode(const std::vector<std::string_view> &args) {
    if (!args.empty() && args[0] == "connect") {
        return SessionMode::Client;
    }
    if (!args.empty() && args[0] == "interactive") {
        return SessionMode::Interactive;
    }
    if (isatty(STDIN_FILENO) == 0) {
        return SessionMode::Pipe;
    }
    if (hasFlag(args, "--")) {
        return SessionMode::Command;
    }
    return SessionMode::Terminal;
}

static std::string sanitizeName(std::string_view raw) {
    std::string out;
    for (char c : raw) {
        auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u != 0x7F) {
            out.push_back(c);
        }
        if (out.size() >= 24) {
            break;
        }
    }
    return out;
}

static std::string urlEncode(std::string_view raw) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (char c : raw) {
        auto u = static_cast<unsigned char>(c);
        bool safe =
            (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~';
        if (safe) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[u >> 4U]);
            out.push_back(hex[u & 0xFU]);
        }
    }
    return out;
}

static std::string localUserName() {
    const char *user = std::getenv("USER");
    if (user == nullptr || *user == '\0') {
        user = std::getenv("LOGNAME");
    }
    return (user != nullptr && *user != '\0') ? user : "guest";
}

static uint16_t g_hostRows = 24;
static uint16_t g_hostCols = 80;

static void renderHostTelemetry(const std::string &room, SessionRegistry &registry,
                                std::unordered_map<std::string, std::shared_ptr<WebRTCSession>> &sessions, std::mutex &sessionsMutex) {
    if (g_hostRows < 2) {
        return;
    }
    size_t count = registry.clientCount();
    long maxRtt = -1;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex);
        for (const auto &[id, sess] : sessions) {
            long r = sess->rttMillis();
            maxRtt = std::max(r, maxRtt);
        }
    }
    std::ostringstream text;
    text << "[AetherProxy Server] Room: " << room;
    text << " · " << count << " clients connected";
    if (maxRtt >= 0) {
        text << " · max rtt " << maxRtt << " ms";
    }
    std::string t = text.str();
    if (g_hostCols > 1 && t.size() > static_cast<size_t>(g_hostCols) - 1) {
        size_t cut = static_cast<size_t>(g_hostCols) - 1;
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        t.resize(cut);
    }
    std::ostringstream line;
    line << "\0337\033[" << g_hostRows << ";1H\033[2K\033[2m" << t << "\033[22m\0338";
    writeFull(STDOUT_FILENO, line.str().data(), line.str().size());
}

static void applyHostWinch(SessionRegistry &registry, const std::string &room,
                           std::unordered_map<std::string, std::shared_ptr<WebRTCSession>> &sessions, std::mutex &sessionsMutex) {
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return;
    }
    g_hostRows = ws.ws_row;
    g_hostCols = ws.ws_col;
    uint16_t remoteRows = ws.ws_row;
    if (g_hostRows >= 2) {
        std::ostringstream region;
        region << "\033[1;" << (g_hostRows - 1) << "r";
        writeFull(STDOUT_FILENO, region.str().data(), region.str().size());
        remoteRows = g_hostRows - 1;
    }
    registry.setBaseSize({.rows = remoteRows, .cols = ws.ws_col});
    renderHostTelemetry(room, registry, sessions, sessionsMutex);
}

static int runInteractiveClient(const CLIConfig &config, const std::string &room) {
    HttpServer server;
    if (!server.bindTo(0, true)) {
        std::cerr << "Could not bind a local port.\n";
        return 1;
    }

    std::atomic<bool> browserDone{false};
    std::atomic<long long> lastPingMs{-1};
    auto startTime = std::chrono::steady_clock::now();
    auto msSince = [startTime]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    };

    std::thread serverThread([&server, &browserDone, &lastPingMs, &msSince]() {
        server.run(nullptr, nullptr, [&browserDone, &lastPingMs, &msSince](const std::string &ev) {
            if (ev == "ping") {
                lastPingMs = msSince();
            } else {
                browserDone = true;
            }
        });
    });

    std::string iceParams;
    if (!config.noStun && !config.stunServers.empty()) {
        std::string joined;
        for (const auto &s : config.stunServers) {
            if (!joined.empty()) {
                joined += ",";
            }
            joined += s;
        }
        iceParams += "&stun=" + urlEncode(joined);
    }
    if (!config.noTurn && !config.turnServers.empty()) {
        iceParams += "&turn=" + urlEncode(config.turnServers.front());
        iceParams += "&turnUser=" + urlEncode(config.turnUser);
        iceParams += "&turnPass=" + urlEncode(config.turnPass);
    }
    std::string url = "http://127.0.0.1:" + std::to_string(server.boundPort()) + "/?signal=" + urlEncode(config.signalingUrl) +
                      "&interactive=1&name=" + urlEncode(localUserName()) + iceParams + "#" + room;
    std::cout << "Opening browser: " << url << "\n" << std::flush;
    launchBrowser(url);

    constexpr long long kConnectTimeoutMs = 60000;
    constexpr long long kPingLossMs = 5000;
    while (!browserDone && g_exitSignal == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        long long last = lastPingMs;
        long long now = msSince();
        if (last < 0 && now > kConnectTimeoutMs) {
            break;
        }
        if (last >= 0 && now - last > kPingLossMs) {
            break;
        }
    }

    server.stop();
    serverThread.join();
    return 0;
}

int main(int argc, char *argv[]) {
    std::vector<std::string_view> args(argv + 1, argv + argc);

    auto delim = std::ranges::find(args, "--");
    std::vector<std::string_view> ownArgs(args.begin(), delim);
    bool wantHelp = hasFlag(ownArgs, "--help") || hasFlag(ownArgs, "-h") || (!args.empty() && args[0] == "help");
    if (wantHelp) {
        printUsage();
        return 0;
    }
    bool wantVersion =
        hasFlag(ownArgs, "--version") || hasFlag(ownArgs, "-V") || hasFlag(ownArgs, "-v") || (!args.empty() && args[0] == "version");
    if (wantVersion) {
        std::cout << "aetherproxy " << versionString() << "\n" << std::flush;
        return 0;
    }
    if (!args.empty() && args[0] == "config") {
        return runConfigCommand(std::vector<std::string>(argv + 2, argv + argc));
    }
    if (!args.empty() && args[0] == "collab") {
        std::cerr << "collab mode is removed.\n"
                  << "Every session accepts multiple clients.\n"
                  << "Set the limit: aetherproxy config set max-clients N\n";
        return 1;
    }

    SessionMode mode = detectMode(args);
    CLIConfig config = parseCLIArgs(argc, argv);
    Logger::init("aetherproxy");

    if (mode == SessionMode::Client && isatty(STDIN_FILENO) == 0) {
        std::cerr << "This mode needs a terminal on stdin.\n";
        return 1;
    }

    if (mode == SessionMode::Client) {
        if (argc < 3) {
            std::cerr << "Usage: aetherproxy connect <room>\n";
            return 1;
        }
        if (config.signalingUrl.empty()) {
            std::cerr << "Error: no signaling server set.\n"
                      << "Run: aetherproxy config set offline false\n";
            return 1;
        }
        NativeClient client;
        client.connect(config, argv[2]);
        client.run();
        return 0;
    }

    if (mode == SessionMode::Interactive) {
        std::string room;
        if (argc >= 3) {
            room = argv[2];
        }
        if (config.signalingUrl.empty()) {
            std::cerr << "Error: no signaling server set.\n"
                      << "Run: aetherproxy config set offline false\n";
            return 1;
        }
        ::signal(SIGINT, handleExitSignal);
        ::signal(SIGTERM, handleExitSignal);
        return runInteractiveClient(config, room);
    }

    ::signal(SIGINT, handleExitSignal);
    ::signal(SIGTERM, handleExitSignal);

    std::string room = generateRoomCode();
    std::string lanIp = getLanIp();
    std::string url = "http://" + lanIp + ":" + std::to_string(config.port) + "/#" + room;
    renderStartupScreen(room, url, mode != SessionMode::Pipe);

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

    SessionRecorder recorder;
    if (!config.recordPath.empty()) {
        struct winsize recWs {};
        uint16_t recCols = 80;
        uint16_t recRows = 24;
        if (isatty(STDOUT_FILENO) != 0 && ioctl(STDOUT_FILENO, TIOCGWINSZ, &recWs) == 0 && recWs.ws_col > 0 && recWs.ws_row > 0) {
            recCols = recWs.ws_col;
            recRows = recWs.ws_row;
        }
        if (!recorder.start(config.recordPath, recCols, recRows)) {
            std::cerr << "Cannot write recording: " << config.recordPath << "\n";
            return 1;
        }
        std::cout << "Recording to " << config.recordPath << "\n" << std::flush;
    }

    std::atomic<bool> running{true};

    std::unordered_map<std::string, std::shared_ptr<WebRTCSession>> sessions;
    std::mutex sessionsMutex;
    const size_t sessionLimit = static_cast<size_t>(std::min(config.maxClients, 32));

    HttpServer server;
    if (!server.bindTo(config.port, false)) {
        return 1;
    }
    std::cout << "Server listening on port " << server.boundPort() << "\n" << std::flush;
    std::thread serverThread([&server, &config, &eq, &sessions, &sessionsMutex, sessionLimit, &running]() {
        server.run(
            [&config, &eq, &sessions, &sessionsMutex, sessionLimit](const std::string &clientId, const std::string &name) {
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
                std::weak_ptr<WebRTCSession> weakSession = session;
                session->onOpen([&eq, clientId, name, weakSession]() {
                    auto s = weakSession.lock();
                    if (!s) {
                        return;
                    }
                    eq.push(
                        Event{.type = Event::Type::Join, .clientId = clientId, .clientName = name, .data = "", .channel = s->getChannel()});
                });
                session->onMessage([&eq, clientId](std::string msg) {
                    eq.push(Event{
                        .type = Event::Type::Message, .clientId = clientId, .clientName = "", .data = std::move(msg), .channel = nullptr});
                });
                session->onDisconnect([&eq, clientId]() {
                    eq.push(Event{.type = Event::Type::Disconnect, .clientId = clientId, .clientName = "", .data = "", .channel = nullptr});
                });
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
        running = false;
    });

    std::shared_ptr<SignalingClient> sigClient;
    if (!config.signalingUrl.empty()) {
        sigClient = std::make_shared<SignalingClient>();
        sigClient->onMessage([&](const std::string &type, const std::string &data) {
            if (type == "hello") {
                std::string clientId;
                std::string name;
                try {
                    auto j = nlohmann::json::parse(data);
                    clientId = j.value("clientId", "");
                    name = j.value("name", "");
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
                std::weak_ptr<WebRTCSession> weakSession = session;
                session->onOpen([&eq, clientId, name, weakSession]() {
                    auto s = weakSession.lock();
                    if (!s) {
                        return;
                    }
                    eq.push(
                        Event{.type = Event::Type::Join, .clientId = clientId, .clientName = name, .data = "", .channel = s->getChannel()});
                });
                session->onMessage([&eq, clientId](std::string msg) {
                    eq.push(Event{
                        .type = Event::Type::Message, .clientId = clientId, .clientName = "", .data = std::move(msg), .channel = nullptr});
                });
                session->onDisconnect([&eq, clientId]() {
                    eq.push(Event{.type = Event::Type::Disconnect, .clientId = clientId, .clientName = "", .data = "", .channel = nullptr});
                });
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
        sigClient->onOpen([&config, room]() {
            std::cout << "Signaling connected: " << config.signalingUrl << "  room=" << room << "\r\n" << std::flush;
        });
        sigClient->onError([](const std::string &reason) {
            std::cout << "Signaling unreachable: " << reason << "\r\nLocal links still work.\r\n" << std::flush;
        });
        try {
            sigClient->connect(config.signalingUrl, room);
        } catch (const std::exception &e) {
            std::cout << "Signaling failed: " << e.what() << "\nLocal links still work.\n" << std::flush;
        }
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
        applyHostWinch(registry, room, sessions, sessionsMutex);
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

    ReplayBuffer replay(2U << 20U);
    bool eofSeen = false;
    constexpr size_t kReplayChunk = 16384;

    const bool admissionGate = config.admit && localTty && mode != SessionMode::Pipe;
    std::deque<std::pair<std::string, std::string>> admitQueue;
    std::string awaitingAdmit;

    auto promptAdmit = [&admitQueue, &awaitingAdmit]() {
        if (!awaitingAdmit.empty() || admitQueue.empty()) {
            return;
        }
        awaitingAdmit = admitQueue.front().first;
        std::string prompt = "\r\n[AetherProxy: Connection request from " + admitQueue.front().second + ". Accept? Y/n] ";
        writeFull(STDOUT_FILENO, prompt.data(), prompt.size());
    };

    auto resolveAdmit = [&](bool accepted) {
        if (accepted) {
            registry.setPermission(awaitingAdmit, Permission::Collaborator);
            writeFull(STDOUT_FILENO, "accepted\r\n", 10);
        } else {
            registry.kickClient(awaitingAdmit, "rejected by host");
            std::lock_guard<std::mutex> lock(sessionsMutex);
            sessions.erase(awaitingAdmit);
            writeFull(STDOUT_FILENO, "rejected\r\n", 10);
        }
        admitQueue.pop_front();
        awaitingAdmit.clear();
        promptAdmit();
    };

    auto dropPending = [&](const std::string &id) {
        std::erase_if(admitQueue, [&](const auto &entry) { return entry.first == id; });
        if (awaitingAdmit == id) {
            writeFull(STDOUT_FILENO, "gone\r\n", 6);
            awaitingAdmit.clear();
            promptAdmit();
        }
    };

    auto lastClientSeen = std::chrono::steady_clock::now();

    while (running && g_exitSignal == 0) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastInactivityCheck).count() >= 5) {
            registry.checkInactivity();
            if (localTty) {
                renderHostTelemetry(room, registry, sessions, sessionsMutex);
            }
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
                    if (!awaitingAdmit.empty()) {
                        for (ssize_t k = 0; k < n; ++k) {
                            char c = buf[k];
                            if (c == 'y' || c == 'Y' || c == '\r' || c == '\n') {
                                resolveAdmit(true);
                                break;
                            }
                            if (c == 'n' || c == 'N') {
                                resolveAdmit(false);
                                break;
                            }
                        }
                    } else {
                        io->write(buf, static_cast<size_t>(n));
                    }
                } else if (n == 0) {
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
                }
            } else if (localTty && events[i].data.fd == winchFd) {
                uint64_t val = 0;
                ssize_t r = read(winchFd, &val, sizeof(val));
                (void)r;
                applyHostWinch(registry, room, sessions, sessionsMutex);
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
                        if (mode == SessionMode::Pipe) {
                            perm = Permission::Observer;
                        } else if (admissionGate) {
                            perm = Permission::Pending;
                        }
                        std::string name = sanitizeName(evItem.clientName);
                        if (name.empty()) {
                            name = evItem.clientId;
                        }
                        bool added = registry.addClient(evItem.clientId, name, perm, evItem.channel);
                        if (added) {
                            Logger::info("Client joined: id=" + evItem.clientId + " name=" + name);
                        }
                        std::string modeStr = mode == SessionMode::Pipe ? "pipe" : "terminal";
                        std::string modeFrame = std::string(
                                                    "\x00"
                                                    "AETHER:",
                                                    8) +
                                                R"({"type":"mode","mode":")" + modeStr + R"("})";
                        if (added && evItem.channel) {
                            evItem.channel->send(modeFrame);
                            std::string snap = replay.snapshot();
                            for (size_t off = 0; off < snap.size(); off += kReplayChunk) {
                                evItem.channel->send(snap.substr(off, kReplayChunk));
                            }
                            if (eofSeen) {
                                evItem.channel->send(std::string("\x00"
                                                                 "AETHER:",
                                                                 8) +
                                                     R"({"type":"eof"})");
                            }
                        }
                        if (added && perm == Permission::Pending) {
                            admitQueue.emplace_back(evItem.clientId, name);
                            promptAdmit();
                        }
                    } else if (evItem.type == Event::Type::Disconnect) {
                        Logger::info("Client disconnected: id=" + evItem.clientId);
                        dropPending(evItem.clientId);
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
                                    registry.updateClientSize(evItem.clientId, {.rows = r, .cols = c});
                                } else if (payload.contains("type") && payload["type"] == "bye") {
                                    dropPending(evItem.clientId);
                                    registry.removeClient(evItem.clientId);
                                    std::lock_guard<std::mutex> lock(sessionsMutex);
                                    sessions.erase(evItem.clientId);
                                }
                            } catch (...) {
                                continue;
                            }
                        } else {
                            registry.handleInput(evItem.clientId, msg, [&io](std::string_view d) { io->write(d.data(), d.size()); });
                        }
                    }
                }
                if (localTty) {
                    renderHostTelemetry(room, registry, sessions, sessionsMutex);
                }
            } else if (events[i].data.fd == io->getFd()) {
                char buf[1024];
                ssize_t n = io->read(buf, sizeof(buf));
                if (n > 0) {
                    registry.broadcastOutput(std::string_view(buf, n));
                    if (localTty) {
                        writeFull(STDOUT_FILENO, buf, static_cast<size_t>(n));
                    }
                    replay.append(std::string_view(buf, static_cast<size_t>(n)));
                    recorder.record(std::string_view(buf, static_cast<size_t>(n)));
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

    if (g_exitSignal != 0) {
        registry.broadcastControl(std::string(R"({"type":"server_exit","reason":"terminated"})"));
    } else {
        registry.broadcastControl(std::string(R"({"type":"end"})"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    recorder.stop();
    server.stop();
    if (serverThread.joinable()) {
        serverThread.join();
    }

    if (localTty) {
        writeFull(STDOUT_FILENO, "\x1b[r", 3);
        disableRawTty();
        if (winchFd != -1) {
            close(winchFd);
        }
        std::cout << "\nSession ended.\n" << std::flush;
    }
    close(epollFd);
    return 0;
}
