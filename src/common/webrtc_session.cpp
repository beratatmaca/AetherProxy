#include "common/webrtc_session.hpp"
#include <chrono>
#include <iostream>
#include <future>
#include <sstream>

WebRTCSession::WebRTCSession() = default;

WebRTCSession::~WebRTCSession() {
    if (dc) {
        dc->close();
    }
    if (pc) {
        pc->close();
    }
}

void WebRTCSession::initialize(const std::vector<std::string> &stunServers, const std::vector<std::string> &turnServers,
                               const std::string &turnUser, const std::string &turnPass, bool noStun, bool noTurn, bool offerer) {
    rtc::Configuration config;
    if (!noStun) {
        for (const auto &s : stunServers) {
            config.iceServers.emplace_back(s);
        }
    }
    if (!noTurn) {
        for (const auto &t : turnServers) {
            rtc::IceServer server(t);
            server.type = rtc::IceServer::Type::Turn;
            server.username = turnUser;
            server.password = turnPass;
            config.iceServers.push_back(server);
        }
    }

    pc = std::make_shared<rtc::PeerConnection>(config);

    pc->onIceStateChange([this](rtc::PeerConnection::IceState state) {
        if (state == rtc::PeerConnection::IceState::Failed || state == rtc::PeerConnection::IceState::Closed) {
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                if (disconnectCallback) {
                    cb = std::move(disconnectCallback);
                    disconnectCallback = nullptr;
                }
            }
            if (cb) {
                cb();
            }
        }
    });

    pc->onStateChange([this](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                if (disconnectCallback) {
                    cb = std::move(disconnectCallback);
                    disconnectCallback = nullptr;
                }
            }
            if (cb) {
                cb();
            }
        }
    });

    pc->onLocalCandidate([this](const rtc::Candidate &cand) {
        std::function<void(std::string, std::string)> cb;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            cb = candidateCallback;
        }
        if (cb) {
            cb(std::string(cand), cand.mid());
        }
    });

    pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
        dc = std::move(channel);
        registerDataChannelCallbacks();
    });

    if (offerer) {
        dc = pc->createDataChannel("aether-data");
        registerDataChannelCallbacks();
    }
}

void WebRTCSession::registerDataChannelCallbacks() {
    if (!dc)
        return;

    dc->onOpen([this]() {
        std::cout << "DataChannel opened. DTLS active." << "\n" << std::flush;
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            cb = openCallback;
        }
        if (cb) {
            cb();
        }
    });

    dc->onClosed([this]() {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            if (disconnectCallback) {
                cb = std::move(disconnectCallback);
                disconnectCallback = nullptr;
            }
        }
        if (cb) {
            cb();
        }
    });

    dc->onMessage([this](rtc::message_variant msg) {
        std::function<void(std::string)> cb;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            cb = messageCallback;
        }
        if (cb) {
            if (std::holds_alternative<std::string>(msg)) {
                cb(std::get<std::string>(msg));
            } else if (std::holds_alternative<rtc::binary>(msg)) {
                auto data = std::get<rtc::binary>(msg);
                std::string str(reinterpret_cast<const char *>(data.data()), data.size());
                cb(str);
            }
        }
    });
}

std::string WebRTCSession::createOffer() {
    if (!pc) {
        return "";
    }
    if (auto existing = pc->localDescription()) {
        return std::string(existing.value());
    }
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    pc->onGatheringStateChange([promise](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            promise->set_value();
        }
    });
    pc->setLocalDescription();
    if (pc->gatheringState() != rtc::PeerConnection::GatheringState::Complete) {
        future.wait_for(std::chrono::seconds(10));
    }

    auto offer = pc->localDescription();
    if (!offer) {
        return "";
    }
    return std::string(offer.value());
}

std::string WebRTCSession::createAnswer() {
    if (!pc) {
        return "";
    }
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    pc->onGatheringStateChange([promise](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            promise->set_value();
        }
    });
    pc->setLocalDescription();
    if (pc->gatheringState() != rtc::PeerConnection::GatheringState::Complete) {
        future.wait_for(std::chrono::seconds(10));
    }

    auto answer = pc->localDescription();
    if (!answer) {
        return "";
    }
    return std::string(answer.value());
}

void WebRTCSession::setOffer(const std::string &sdp) {
    if (pc) {
        pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));
    }
}

void WebRTCSession::setAnswer(const std::string &sdp) {
    if (pc) {
        pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
    }
}

void WebRTCSession::addCandidate(const std::string &sdp, const std::string &mid) {
    if (pc) {
        pc->addRemoteCandidate(rtc::Candidate(sdp, mid));
    }
}

void WebRTCSession::send(const std::string &msg) {
    if (dc && dc->isOpen()) {
        dc->send(msg);
    }
}

void WebRTCSession::onMessage(std::function<void(std::string)> cb) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    messageCallback = std::move(cb);
}

void WebRTCSession::onDisconnect(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    disconnectCallback = std::move(cb);
}

void WebRTCSession::onOpen(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    openCallback = std::move(cb);
}

std::shared_ptr<rtc::DataChannel> WebRTCSession::getChannel() const {
    return dc;
}

long WebRTCSession::rttMillis() {
    if (!pc) {
        return -1;
    }
    auto rtt = pc->rtt();
    return rtt ? static_cast<long>(rtt->count()) : -1;
}

std::string WebRTCSession::stateString() {
    if (!pc) {
        return "closed";
    }
    std::ostringstream oss;
    oss << pc->state();
    return oss.str();
}

void WebRTCSession::onCandidate(std::function<void(std::string sdp, std::string mid)> cb) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    candidateCallback = std::move(cb);
}
