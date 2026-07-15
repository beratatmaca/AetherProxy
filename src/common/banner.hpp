#pragma once
#include <string>

/// Renders the startup banner and QR code sized to the terminal.
void renderStartupScreen(const std::string &room, const std::string &url, bool localShell);
