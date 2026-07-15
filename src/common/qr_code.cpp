#include "common/qr_code.hpp"
#include <qrencode.h>
#include <iostream>

std::vector<std::string> qrCodeLines(const std::string &text, bool compact) {
    std::vector<std::string> lines;
    QRcode *qr = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) {
        return lines;
    }

    int width = qr->width;
    const int quiet = 2;
    int total = width + 2 * quiet;

    auto light = [&](int x, int y) {
        int mx = x - quiet;
        int my = y - quiet;
        if (mx < 0 || my < 0 || mx >= width || my >= width) {
            return true;
        }
        return (qr->data[my * width + mx] & 1) == 0;
    };

    if (compact) {
        for (int y = 0; y < total; y += 2) {
            std::string line;
            for (int x = 0; x < total; ++x) {
                bool top = light(x, y);
                bool bottom = (y + 1 < total) ? light(x, y + 1) : true;
                if (top && bottom) {
                    line += "█";
                } else if (top) {
                    line += "▀";
                } else if (bottom) {
                    line += "▄";
                } else {
                    line += " ";
                }
            }
            lines.push_back(line);
        }
    } else {
        for (int y = 0; y < total; ++y) {
            std::string line;
            for (int x = 0; x < total; ++x) {
                line += light(x, y) ? "██" : "  ";
            }
            lines.push_back(line);
        }
    }

    QRcode_free(qr);
    return lines;
}

void renderQRCode(const std::string &text) {
    std::cout << "\n";
    for (const auto &line : qrCodeLines(text, false)) {
        std::cout << "  " << line << "\n";
    }
    std::cout << "\n";
}
