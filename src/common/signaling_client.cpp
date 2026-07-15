#include "common/signaling_client.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

SignalingClient::SignalingClient() : ws(std::make_shared<rtc::WebSocket>()) {}

SignalingClient::~SignalingClient() {
    if (ws) {
        ws->close();
    }
}

void SignalingClient::connect(const std::string& url, const std::string& roomCode) {
    room = roomCode;

    ws->onOpen([this]() {
        json joinMsg = {
            {"type", "join"},
            {"room", room}
        };
        ws->send(joinMsg.dump());
    });

    ws->onMessage([this](rtc::message_variant data) {
        if (!std::holds_alternative<std::string>(data)) {
            return;
        }
        std::string raw = std::get<std::string>(data);
        try {
            json msg = json::parse(raw);
            if (msg.contains("type") && msg.contains("data")) {
                std::string msgType = msg["type"];
                std::string msgData = msg["data"];
                if (messageCallback) {
                    messageCallback(msgType, msgData);
                }
            }
        } catch (...) {
        }
    });

    ws->open(url);
}

void SignalingClient::send(const std::string& type, const std::string& data) {
    if (!ws || !ws->isOpen()) {
        return;
    }
    json msg = {
        {"room", room},
        {"type", type},
        {"data", data}
    };
    ws->send(msg.dump());
}

void SignalingClient::onMessage(std::function<void(std::string type, std::string data)> cb) {
    messageCallback = std::move(cb);
}
