#pragma once
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <rtc/peerconnection.hpp>
#include <rtc/datachannel.hpp>

/// Manages a WebRTC connection session.
class WebRTCSession {
public:
    /// Creates the WebRTC session.
    WebRTCSession();

    /// Cleans up peer connection.
    ~WebRTCSession();

    /// Initializes connection using signaling parameters.
    /// @param stunServers List of STUN servers.
    /// @param turnServers List of TURN servers.
    /// @param turnUser TURN username.
    /// @param turnPass TURN password.
    /// @param noStun Disable STUN if true.
    /// @param noTurn Disable TURN if true.
    /// @param offerer True if creating data channel.
    void initialize(const std::vector<std::string> &stunServers, const std::vector<std::string> &turnServers, const std::string &turnUser,
                    const std::string &turnPass, bool noStun = false, bool noTurn = false, bool offerer = true);

    /// Generates local SDP offer.
    std::string createOffer();

    /// Generates local SDP answer.
    std::string createAnswer();

    /// Sets remote SDP offer.
    void setOffer(const std::string &sdp);

    /// Sets remote SDP answer.
    void setAnswer(const std::string &sdp);

    /// Adds remote ICE candidate.
    void addCandidate(const std::string &sdp, const std::string &mid);

    /// Sends message via DataChannel.
    void send(const std::string &msg);

    /// Sets message reception callback.
    void onMessage(std::function<void(std::string)> cb);

    /// Sets connection open callback.
    void onOpen(std::function<void()> cb);

    /// Gets underlying data channel.
    std::shared_ptr<rtc::DataChannel> getChannel() const;

    /// Returns round trip time in ms, -1 unknown.
    long rttMillis();

    /// Returns the connection state name.
    std::string stateString();

    /// Sets ICE failure callback.
    void onDisconnect(std::function<void()> cb);

    /// Sets local ICE candidate callback.
    void onCandidate(std::function<void(std::string sdp, std::string mid)> cb);

private:
    void registerDataChannelCallbacks();

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::function<void(std::string)> messageCallback;
    std::function<void()> disconnectCallback;
    std::function<void(std::string sdp, std::string mid)> candidateCallback;
    std::function<void()> openCallback;
    std::mutex callbackMutex;
};
