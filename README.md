# AetherProxy

**Share your terminal to any phone. Scan a QR. Done.**

<div align="center">

[![CI](https://github.com/beratatmaca/AetherProxy/actions/workflows/ci.yml/badge.svg)](https://github.com/beratatmaca/AetherProxy/actions/workflows/ci.yml)
[![Release](https://github.com/beratatmaca/AetherProxy/actions/workflows/release.yml/badge.svg)](https://github.com/beratatmaca/AetherProxy/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

</div>

---

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/beratatmaca/AetherProxy/main/tools/install.sh | bash
```

Or grab a `.deb` from the [latest release](https://github.com/beratatmaca/AetherProxy/releases/latest):

```bash
sudo dpkg -i aetherproxy_*_amd64.deb
```

---

## What it does

AetherProxy creates a zero-config, peer-to-peer bridge between any Linux terminal and any browser — over an encrypted WebRTC DataChannel.

- **Run it.** A QR code appears in your terminal.
- **Scan it.** Your phone opens a full xterm.js terminal in the browser.
- **Type.** Keystrokes stream back to the host in real time — fully interactive, vim and htop included.

No server. No cloud account. No configuration file. The signaling is done entirely over LAN by default.

---

## Usage

### Share your terminal (local network)

```bash
aetherproxy
```

Prints a room code and a QR code encoding `http://<your-LAN-IP>:<port>/#<room>`.  
Scan with your phone or open the URL in any browser on the same network.

### Run a specific command

```bash
aetherproxy -- htop
aetherproxy -- bash -c 'watch -n1 df -h'
```

### Pipe mode — stream a file or command output (read-only)

```bash
cat /var/log/syslog | aetherproxy
curl -s https://example.com | aetherproxy
journalctl -f | aetherproxy
```

The browser receives the output live. Keyboard input is disabled.

### Internet mode — share across networks

Requires a WebSocket signaling server (e.g. `tools/signaling-server.js`):

```bash
# On the host:
aetherproxy --signal wss://your-signal-server.com --stun stun:stun.l.google.com:19302

# On the client (native):
aetherproxy connect <room-code> --signal wss://your-signal-server.com
```

### Native client

```bash
aetherproxy connect granite-torch-river-42017 --signal wss://your-signal-server.com
```

Connects from another Linux machine; raw mode passthrough, full resize support.

---

## Options

| Flag | Default | Description |
|---|---|---|
| `--port <n>` | `8080` | Local HTTP signaling port |
| `--signal <url>` | — | WebSocket signaling server (enables internet mode) |
| `--stun <url>` | — | STUN server URL |
| `--turn <url>` | — | TURN server URL |
| `--turn-user <s>` | — | TURN username |
| `--turn-pass <s>` | — | TURN password |
| `--no-stun` | — | Disable STUN (local only) |
| `--no-turn` | — | Disable TURN relay |
| `--max-clients <n>` | `10` | Max simultaneous WebRTC peers |

---

## How it works

```txt
┌─────────────────────────────────────────────────────┐
│  Host (Linux)                                       │
│                                                     │
│  PTY / stdin  ──►  SessionRegistry  ──►  WebRTC    │
│                          │                  │       │
│               Resize, Presence, EOF         │       │
│                     frames                  │       │
└─────────────────────────────────────────────┼───────┘
                                              │  DTLS/SCTP DataChannel
                                     ─────── │ ──────────────────────
┌─────────────────────────────────────────────┼───────┐
│  Browser / Native Client                    │       │
│                                             ▼       │
│  xterm.js  ◄───────────────────────  WebRTC DC     │
│                                                     │
│  Resize frames → host PTY (TIOCSWINSZ)              │
│  Ctrl/Alt/Tab toolbar, pipe-mode lock               │
└─────────────────────────────────────────────────────┘
```

- Transport: **WebRTC DataChannel** with DTLS 1.3 (via `libdatachannel`)
- Local mode: ephemeral HTTP server for SDP offer/answer exchange — **zero internet dependency**
- PTY: `forkpty` + `epoll`; resize tracked via `SIGWINCH` / `TIOCGWINSZ`
- Collaborative: up to 32 concurrent peers with Owner / Collaborator / Observer permissions
- Terminal sync: lowest-common-denominator terminal size broadcast to all clients

---

## Build from source

```bash
git clone https://github.com/beratatmaca/AetherProxy.git
cd AetherProxy

# Embed web assets (xterm.js, CSS, HTML → C++ header)
bash tools/embed-assets.sh

# Configure and build
cmake --preset gcc-release -G Ninja
cmake --build --preset gcc-release

./build-gcc-release/aetherproxy
```

**Requirements:** GCC 12+, CMake 3.20+, Ninja, libssl-dev, libsrtp2-dev.  
All other dependencies (`libdatachannel`, `nlohmann/json`, `libqrencode`) are fetched automatically by CMake.

### Static binary

```bash
cmake --preset gcc-release -G Ninja -DSTATIC_BUILD=ON
cmake --build --preset gcc-release
```

### Sanitizer builds

```bash
# AddressSanitizer
cmake --preset gcc-asan -G Ninja && cmake --build --preset gcc-asan

# ThreadSanitizer (Clang)
cmake --preset clang-tsan -G Ninja && cmake --build --preset clang-tsan
```

---

## Running tests

```bash
cd tests && npm install && npx playwright install chromium firefox
cd ..
bash tools/run-tests.sh          # Playwright integration suite (T-01 … T-12)
bash tools/run-tests.sh --asan   # Build + ASan smoke test
bash tools/run-tests.sh --tsan   # Build + TSan smoke test
```

---

## License

MIT — see [LICENSE](LICENSE).
