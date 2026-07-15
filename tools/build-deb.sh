#!/usr/bin/env bash
# =============================================================================
# AetherProxy — Debian package builder
# Usage: ./tools/build-deb.sh [--arch <amd64|arm64>] [--binary <path>]
#
# Examples:
#   ./tools/build-deb.sh
#   ./tools/build-deb.sh --arch arm64 --binary cross-build/aetherproxy-arm64
# =============================================================================
set -euo pipefail

# ###########################################################################
# Colours
# ###########################################################################
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
RESET='\033[0m'

ok()   { echo -e "${GREEN}✓${RESET} $*"; }
err()  { echo -e "${RED}✗${RESET} $*" >&2; }
info() { echo -e "${YELLOW}→${RESET} $*"; }

# ###########################################################################
# Locate repo root (the directory that contains this script's parent)
# ###########################################################################
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ###########################################################################
# Argument defaults
# ###########################################################################
RAW_ARCH="$(uname -m)"
case "${RAW_ARCH}" in
  x86_64)          DEFAULT_ARCH="amd64" ;;
  aarch64 | arm64) DEFAULT_ARCH="arm64" ;;
  *)               DEFAULT_ARCH="amd64" ;;   # best-effort fallback
esac

ARCH="${DEFAULT_ARCH}"
BINARY=""   # resolved below after arg parsing

# ###########################################################################
# Argument parsing
# ###########################################################################
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)
      shift
      ARCH="${1:?'--arch requires a value: amd64 or arm64'}"
      shift
      ;;
    --binary)
      shift
      BINARY="${1:?'--binary requires a path'}"
      shift
      ;;
    -h | --help)
      echo "Usage: $0 [--arch <amd64|arm64>] [--binary <path>]"
      exit 0
      ;;
    *)
      err "Unknown argument: $1"
      echo "Usage: $0 [--arch <amd64|arm64>] [--binary <path>]" >&2
      exit 1
      ;;
  esac
done

# Validate arch
case "${ARCH}" in
  amd64 | arm64) ;;
  *)
    err "Unsupported architecture: ${ARCH}. Use 'amd64' or 'arm64'."
    exit 1
    ;;
esac

# Default binary path (native build output)
if [[ -z "${BINARY}" ]]; then
  BINARY="${REPO_ROOT}/build-gcc-release/aetherproxy"
fi

# ###########################################################################
# Dependency check
# ###########################################################################
if ! command -v dpkg-deb &>/dev/null; then
  err "dpkg-deb is required but was not found."
  err "Install it with:  sudo apt-get install dpkg"
  exit 1
fi

# ###########################################################################
# Read version
# ###########################################################################
VERSION_FILE="${REPO_ROOT}/VERSION"

if [[ ! -f "${VERSION_FILE}" ]]; then
  err "VERSION file not found at: ${VERSION_FILE}"
  err "Create it with the desired version string, e.g.:  echo '0.1.0' > VERSION"
  exit 1
fi

VERSION="$(tr -d '[:space:]' < "${VERSION_FILE}")"

if [[ -z "${VERSION}" ]]; then
  err "VERSION file is empty. Please add a version string (e.g. 0.1.0)."
  exit 1
fi

info "Building AetherProxy ${BOLD}v${VERSION}${RESET} for ${BOLD}${ARCH}${RESET}"

# ###########################################################################
# Validate binary
# ###########################################################################
if [[ ! -f "${BINARY}" ]]; then
  err "Binary not found at: ${BINARY}"
  err "Build it first, or pass the correct path with --binary <path>."
  exit 1
fi

if [[ ! -x "${BINARY}" ]]; then
  err "Binary is not executable: ${BINARY}"
  err "Run:  chmod +x ${BINARY}"
  exit 1
fi

BINARY_SIZE="$(wc -c < "${BINARY}")"
if [[ "${BINARY_SIZE}" -le 1000 ]]; then
  err "Binary at ${BINARY} is suspiciously small (${BINARY_SIZE} bytes)."
  err "It may be an incomplete build. Aborting."
  exit 1
fi

info "Binary: ${BINARY} (${BINARY_SIZE} bytes)"

# ###########################################################################
# Prepare staging directory
# ###########################################################################
PACKAGE_NAME="aetherproxy_${VERSION}_${ARCH}"
DIST_DIR="${REPO_ROOT}/dist"
STAGING_DIR="${DIST_DIR}/${PACKAGE_NAME}"

# Clean any previous staging area for this version+arch
rm -rf "${STAGING_DIR}"

info "Creating package staging directory at: ${STAGING_DIR}"

# Directory tree
mkdir -p \
  "${STAGING_DIR}/DEBIAN" \
  "${STAGING_DIR}/usr/local/bin" \
  "${STAGING_DIR}/usr/share/doc/aetherproxy"

# ###########################################################################
# DEBIAN/control
# ###########################################################################
cat > "${STAGING_DIR}/DEBIAN/control" <<EOF
Package: aetherproxy
Version: ${VERSION}
Architecture: ${ARCH}
Maintainer: Berat Atmaca <berat@aetherproxy.dev>
Section: net
Priority: optional
Installed-Size: $((BINARY_SIZE / 1024 + 1))
Depends:
Description: Share your Linux terminal to any browser over WebRTC
 AetherProxy bridges any Linux PTY or stdin stream to a browser via an
 encrypted WebRTC DataChannel. Scan a QR code with your phone and get a
 fully interactive xterm.js terminal — no cloud, no config, no server.
 Supports pipe mode, collaborative sessions, and native client mode.
EOF

ok "Created DEBIAN/control"

# ###########################################################################
# DEBIAN/postinst
# ###########################################################################
cat > "${STAGING_DIR}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
chmod +x /usr/local/bin/aetherproxy
EOF

chmod 0755 "${STAGING_DIR}/DEBIAN/postinst"
ok "Created DEBIAN/postinst"

# ###########################################################################
# Binary
# ###########################################################################
cp "${BINARY}" "${STAGING_DIR}/usr/local/bin/aetherproxy"
chmod 0755 "${STAGING_DIR}/usr/local/bin/aetherproxy"
ok "Copied binary → usr/local/bin/aetherproxy"

# ###########################################################################
# Copyright notice
# ###########################################################################
cat > "${STAGING_DIR}/usr/share/doc/aetherproxy/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: AetherProxy
Upstream-Contact: Berat Atmaca <berat@aetherproxy.dev>
Source: https://github.com/beratatmaca/AetherProxy

Files: *
Copyright: $(date +%Y) Berat Atmaca
License: MIT

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
EOF

ok "Created usr/share/doc/aetherproxy/copyright"

# ###########################################################################
# Build the .deb
# ###########################################################################
DEB_FILE="${DIST_DIR}/${PACKAGE_NAME}.deb"

info "Running dpkg-deb --build …"
dpkg-deb --build --root-owner-group "${STAGING_DIR}" "${DEB_FILE}"

# ###########################################################################
# Verify output
# ###########################################################################
if [[ ! -f "${DEB_FILE}" ]]; then
  err "dpkg-deb finished but the output file was not found: ${DEB_FILE}"
  exit 1
fi

DEB_SIZE="$(wc -c < "${DEB_FILE}")"

# Clean up staging dir (keep dist/)
rm -rf "${STAGING_DIR}"

# ###########################################################################
# Success
# ###########################################################################
echo
ok "${BOLD}Debian package built successfully!${RESET}"
ok "Path : ${DEB_FILE}"
ok "Size : ${DEB_SIZE} bytes"
echo
echo -e "  Install with:  ${BOLD}sudo dpkg -i ${DEB_FILE}${RESET}"
