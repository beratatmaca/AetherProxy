#pragma once
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include <cstdint>
#include <rtc/datachannel.hpp>

/// Client permission levels.
enum class Permission : std::uint8_t { Collaborator, Observer };

/// Terminal size specifications.
struct TermSize {
    uint16_t rows;
    uint16_t cols;
};

/// Active peer client representation.
struct Client {
    std::string id;
    std::string displayName;
    std::string color;
    Permission permission;
    bool active;
    TermSize termSize;
    std::shared_ptr<rtc::DataChannel> channel;
    std::chrono::steady_clock::time_point lastActivity;
    std::chrono::steady_clock::time_point lastLockNotify;
};

/// Tracks active session peers.
class SessionRegistry {
public:
    /// Creates the registry.
    SessionRegistry(int ptyFd, int maxClientsVal);

    /// Registers a client.
    void addClient(const std::string &id, const std::string &name, Permission perm, const std::shared_ptr<rtc::DataChannel> &chan);

    /// Unregisters a client.
    void removeClient(const std::string &id);

    /// Updates client size.
    void updateClientSize(const std::string &id, TermSize size);

    /// Broadcasts terminal output.
    void broadcastOutput(std::string_view data);

    /// Broadcasts control messages.
    void broadcastControl(const std::string &msg);

    /// Processes client input data.
    void handleInput(const std::string &id, std::string_view data, const std::function<void(std::string_view)> &onPtyWrite);

    /// Updates client activity.
    void updateActivity(const std::string &id);

    /// Checks inactive clients.
    void checkInactivity();

    /// Returns connected client count.
    size_t clientCount() const;

    /// Sets the host terminal size floor.
    void setBaseSize(TermSize size);

private:
    TermSize calcLCDSize() const;
    void applyLCDSize();
    void broadcastPresence();
    void notifyLocked(Client &client, const std::string &byName);

    std::vector<Client> clients;
    TermSize baseSize{24, 80};
    std::string writerId;
    std::chrono::steady_clock::time_point writerAt;
    int pty;
    int maxClients;
};
