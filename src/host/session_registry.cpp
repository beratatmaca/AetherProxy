#include "host/session_registry.hpp"
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static const std::vector<std::string> COLORS = {"#ef4444", "#3b82f6", "#10b981", "#f59e0b", "#8b5cf6", "#ec4899"};

SessionRegistry::SessionRegistry(int ptyFd, int maxClientsVal) : pty(ptyFd) {
    maxClients = std::min(maxClientsVal, 32);
}

bool SessionRegistry::addClient(const std::string &id, const std::string &name, Permission perm,
                                const std::shared_ptr<rtc::DataChannel> &chan) {
    if (clients.size() >= static_cast<size_t>(maxClients)) {
        if (chan && chan->isOpen()) {
            json rejectMsg = {{"type", "error"}, {"message", "session full"}};
            std::string errStr = std::string(
                                     "\x00"
                                     "AETHER:",
                                     8) +
                                 rejectMsg.dump();
            chan->send(errStr);
            chan->close();
        }
        return false;
    }
    std::string color = COLORS[clients.size() % COLORS.size()];
    for (const auto &candidate : COLORS) {
        bool used = std::ranges::any_of(clients, [&](const Client &c) { return c.color == candidate; });
        if (!used) {
            color = candidate;
            break;
        }
    }
    Client client{.id = id,
                  .displayName = name,
                  .color = color,
                  .permission = perm,
                  .active = true,
                  .termSize = {.rows = 24, .cols = 80},
                  .channel = chan,
                  .lastActivity = std::chrono::steady_clock::now(),
                  .lastLockNotify = {}};
    clients.push_back(client);
    applyLCDSize();
    broadcastPresence();
    return true;
}

void SessionRegistry::removeClient(const std::string &id) {
    std::erase_if(clients, [&](const Client &c) { return c.id == id; });
    applyLCDSize();
    broadcastPresence();
}

void SessionRegistry::setPermission(const std::string &id, Permission perm) {
    for (auto &client : clients) {
        if (client.id == id) {
            client.permission = perm;
            broadcastPresence();
            break;
        }
    }
}

void SessionRegistry::kickClient(const std::string &id, const std::string &reason) {
    for (auto &client : clients) {
        if (client.id == id) {
            if (client.channel && client.channel->isOpen()) {
                json msg = {{"type", "error"}, {"message", reason}};
                client.channel->send(std::string("\x00"
                                                 "AETHER:",
                                                 8) +
                                     msg.dump());
                client.channel->close();
            }
            break;
        }
    }
    removeClient(id);
}

void SessionRegistry::updateClientSize(const std::string &id, TermSize size) {
    for (auto &client : clients) {
        if (client.id == id) {
            client.termSize = size;
            client.sized = true;
            break;
        }
    }
    applyLCDSize();
}

void SessionRegistry::broadcastOutput(std::string_view data) {
    for (auto &client : clients) {
        if (client.channel && client.channel->isOpen()) {
            client.channel->send(std::string(data));
        }
    }
}

void SessionRegistry::broadcastControl(const std::string &msg) {
    std::string formatted = std::string(
                                "\x00"
                                "AETHER:",
                                8) +
                            msg;
    for (auto &client : clients) {
        if (client.channel && client.channel->isOpen()) {
            client.channel->send(formatted);
        }
    }
}

void SessionRegistry::handleInput(const std::string &id, std::string_view data, const std::function<void(std::string_view)> &onPtyWrite) {
    constexpr long kDebounceMs = 500;
    auto now = std::chrono::steady_clock::now();
    for (auto &client : clients) {
        if (client.id != id) {
            continue;
        }
        if (client.permission != Permission::Collaborator) {
            return;
        }
        if (id != writerId && !writerId.empty()) {
            auto held = std::chrono::duration_cast<std::chrono::milliseconds>(now - writerAt).count();
            auto writer = std::ranges::find_if(clients, [this](const Client &c) { return c.id == writerId; });
            if (writer != clients.end() && held < kDebounceMs) {
                notifyLocked(client, writer->displayName);
                return;
            }
        }
        writerId = id;
        writerAt = now;
        onPtyWrite(data);
        updateActivity(id);
        break;
    }
}

void SessionRegistry::notifyLocked(Client &client, const std::string &byName) {
    constexpr long kNotifyIntervalMs = 1000;
    auto now = std::chrono::steady_clock::now();
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - client.lastLockNotify).count();
    if (since < kNotifyIntervalMs) {
        return;
    }
    client.lastLockNotify = now;
    if (client.channel && client.channel->isOpen()) {
        json msg = {{"type", "locked"}, {"by", byName}};
        client.channel->send(std::string("\x00"
                                         "AETHER:",
                                         8) +
                             msg.dump());
    }
}

void SessionRegistry::updateActivity(const std::string &id) {
    bool stateChanged = false;
    for (auto &client : clients) {
        if (client.id == id) {
            client.lastActivity = std::chrono::steady_clock::now();
            if (!client.active) {
                client.active = true;
                stateChanged = true;
            }
            break;
        }
    }
    if (stateChanged) {
        broadcastPresence();
    }
}

void SessionRegistry::checkInactivity() {
    auto now = std::chrono::steady_clock::now();
    bool stateChanged = false;
    for (auto &client : clients) {
        auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - client.lastActivity).count();
        if (idle > 30 && client.active) {
            client.active = false;
            stateChanged = true;
        }
    }
    if (stateChanged) {
        broadcastPresence();
    }
}

size_t SessionRegistry::clientCount() const {
    return clients.size();
}

void SessionRegistry::setBaseSize(TermSize size) {
    if (size.rows > 0 && size.cols > 0) {
        baseSize = size;
        applyLCDSize();
    }
}

TermSize SessionRegistry::calcLCDSize() const {
    uint16_t rows = baseSize.rows;
    uint16_t cols = baseSize.cols;
    for (const auto &client : clients) {
        if (!client.sized) {
            continue;
        }
        rows = std::min(rows, client.termSize.rows);
        cols = std::min(cols, client.termSize.cols);
    }
    return {.rows = rows, .cols = cols};
}

void SessionRegistry::applyLCDSize() {
    auto size = calcLCDSize();
    struct winsize ws {
        .ws_row = size.rows, .ws_col = size.cols, .ws_xpixel = 0, .ws_ypixel = 0
    };
    if (pty != -1) {
        ioctl(pty, TIOCSWINSZ, &ws);
    }
    json msg = {{"type", "resize"}, {"rows", size.rows}, {"cols", size.cols}};
    broadcastControl(msg.dump());
}

void SessionRegistry::broadcastPresence() {
    json list = json::array();
    for (const auto &client : clients) {
        std::string permStr = "pending";
        if (client.permission == Permission::Collaborator) {
            permStr = "collaborator";
        } else if (client.permission == Permission::Observer) {
            permStr = "observer";
        }
        list.push_back({{"id", client.id},
                        {"displayName", client.displayName},
                        {"permission", permStr},
                        {"active", client.active},
                        {"color", client.color}});
    }
    json msg = {{"type", "presence"}, {"clients", list}};
    broadcastControl(msg.dump());
}
