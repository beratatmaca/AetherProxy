#include "common/banner.hpp"
#include "common/qr_code.hpp"
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <vector>

static size_t displayWidth(const std::string &s) {
    size_t w = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            ++w;
        }
    }
    return w;
}

static std::vector<std::string> logoLines() {
    return {
        "▄▀█ █▀▀ ▀█▀ █ █ █▀▀ █▀█",
        "█▀█ ██▄  █  █▀█ ██▄ █▀▄",
        "█▀█ █▀█ █▀█ ▀▄▀ █▄█",
        "█▀▀ █▀▄ █▄█ █ █  █ ",
    };
}

static std::pair<int, int> terminalSize() {
    struct winsize ws {};
    if (isatty(STDOUT_FILENO) != 0 && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        return {ws.ws_row, ws.ws_col};
    }
    return {24, 80};
}

void renderStartupScreen(const std::string &room, const std::string &url) {
    auto [rows, cols] = terminalSize();

    std::vector<std::string> info = logoLines();
    info.emplace_back("");
    info.push_back("Room:  " + room);
    info.push_back("Link:  " + url);
    info.emplace_back("");
    info.emplace_back("Scan the QR code or open the link.");

    size_t leftWidth = 0;
    for (const auto &line : info) {
        leftWidth = std::max(leftWidth, displayWidth(line));
    }

    std::vector<std::string> qrFull = qrCodeLines(url, false);
    std::vector<std::string> qrHalf = qrCodeLines(url, true);
    size_t fullWidth = qrFull.empty() ? 0 : displayWidth(qrFull[0]);
    size_t halfWidth = qrHalf.empty() ? 0 : displayWidth(qrHalf[0]);

    auto c = static_cast<size_t>(cols);
    auto r = static_cast<size_t>(rows);

    const std::vector<std::string> *qr = &qrHalf;
    bool sideBySide = false;
    if (c >= leftWidth + 3 + fullWidth && r >= qrFull.size() + 2) {
        qr = &qrFull;
        sideBySide = true;
    } else if (c >= leftWidth + 3 + halfWidth) {
        qr = &qrHalf;
        sideBySide = true;
    } else if (c >= fullWidth && r >= qrFull.size() + info.size() + 3) {
        qr = &qrFull;
    }

    std::cout << "\n";
    if (sideBySide) {
        size_t total = std::max(info.size(), qr->size());
        size_t infoPad = (qr->size() > info.size()) ? (qr->size() - info.size()) / 2 : 0;
        for (size_t i = 0; i < total; ++i) {
            std::string left;
            if (i >= infoPad && i - infoPad < info.size()) {
                left = info[i - infoPad];
            }
            left.append(leftWidth - displayWidth(left), ' ');
            std::string right = (i < qr->size()) ? (*qr)[i] : "";
            std::cout << " " << left << "  " << right << "\n";
        }
    } else {
        for (const auto &line : info) {
            std::cout << " " << line << "\n";
        }
        std::cout << "\n";
        for (const auto &line : *qr) {
            std::cout << " " << line << "\n";
        }
    }
    std::cout << "\n" << std::flush;
}
