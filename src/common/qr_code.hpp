#pragma once
#include <string>
#include <vector>

/// Returns QR code lines. Compact halves the height.
std::vector<std::string> qrCodeLines(const std::string &text, bool compact);

/// Renders a full-size terminal QR code.
void renderQRCode(const std::string &text);
