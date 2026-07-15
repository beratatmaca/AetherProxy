#include "common/qr_code.hpp"
#include <qrencode.h>
#include <iostream>

void renderQRCode(const std::string& text) {
    QRcode* qr = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr) {
        return;
    }

    int width = qr->width;
    unsigned char* data = qr->data;

    std::cout << "\n";
    for (int y = -2; y < width + 2; ++y) {
        std::cout << "  ";
        for (int x = -2; x < width + 2; ++x) {
            bool black = false;
            if (x >= 0 && x < width && y >= 0 && y < width) {
                black = (data[y * width + x] & 1);
            }
            if (black) {
                std::cout << "\u2588\u2588";
            } else {
                std::cout << "  ";
            }
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    QRcode_free(qr);
}
