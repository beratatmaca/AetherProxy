# AetherProxy

**Share your terminal to any phone. Scan a QR. Done.**

<div align="center">

[![CI](https://github.com/beratatmaca/AetherProxy/actions/workflows/ci.yml/badge.svg)](https://github.com/beratatmaca/AetherProxy/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/beratatmaca/AetherProxy)](https://github.com/beratatmaca/AetherProxy/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE.txt)

</div>

![demo](assets/demo.gif)

*Run it. Scan the QR. Type from your phone.*

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/beratatmaca/AetherProxy/main/tools/install.sh | bash
```

```text
✓ AetherProxy v0.2.3 installed successfully!
```

## Usage

```bash
# Share your shell
aetherproxy

# Stream logs, read only
tail -f app.log | aetherproxy

# Join from another machine
aetherproxy connect fox-river-stone-48291

# Record while you share
aetherproxy --record demo.cast
```

## Features

- Zero setup. One command.
- No account. No cloud.
- Works offline on your LAN.
- One static binary.
- Full TUI apps: vim, htop, tmux.
- Many viewers. You approve each one.
- Late joiners see the scrollback.
- Encrypted end to end.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
