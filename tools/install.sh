#!/usr/bin/env bash
# =============================================================================
# AetherProxy — One-liner installer
# Usage: curl -fsSL https://raw.githubusercontent.com/beratatmaca/AetherProxy/main/tools/install.sh | bash
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
# Dependency check
# ###########################################################################
if ! command -v curl &>/dev/null; then
  err "curl is required but was not found in PATH."
  err "Install it with:  sudo apt-get install curl   (Debian/Ubuntu)"
  err "                  sudo yum install curl        (RHEL/CentOS)"
  exit 1
fi

# ###########################################################################
# Architecture detection
# ###########################################################################
RAW_ARCH="$(uname -m)"
case "${RAW_ARCH}" in
  x86_64)          ARCH="amd64" ;;
  aarch64 | arm64) ARCH="arm64" ;;
  *)
    err "Unsupported architecture: ${RAW_ARCH}"
    err "AetherProxy only supports x86_64 and aarch64/arm64."
    exit 1
    ;;
esac

info "Detected architecture: ${RAW_ARCH} → ${ARCH}"

# ###########################################################################
# Resolve latest release tag
# ###########################################################################
RELEASES_URL="https://api.github.com/repos/beratatmaca/AetherProxy/releases/latest"

info "Fetching latest release information from GitHub…"

API_RESPONSE="$(curl -fsSL \
  -H "Accept: application/vnd.github+json" \
  "${RELEASES_URL}" 2>&1)" || {
  err "Failed to reach GitHub API."
  err "Check your internet connection and try again."
  exit 1
}

TAG="$(echo "${API_RESPONSE}" \
  | grep '"tag_name"' \
  | sed -E 's/.*"tag_name"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')"

if [[ -z "${TAG}" ]]; then
  err "Could not parse the latest release tag from GitHub API response."
  err "Response snippet: $(echo "${API_RESPONSE}" | head -5)"
  exit 1
fi

info "Latest release: ${BOLD}${TAG}${RESET}"

# ###########################################################################
# Download binary
# ###########################################################################
BINARY_NAME="aetherproxy-linux-${ARCH}"
DOWNLOAD_URL="https://github.com/beratatmaca/AetherProxy/releases/download/${TAG}/${BINARY_NAME}"
TMP_FILE="$(mktemp /tmp/aetherproxy.XXXXXX)"

# Ensure temp file is removed on exit regardless of outcome
trap 'rm -f "${TMP_FILE}"' EXIT

info "Downloading ${BINARY_NAME} from:"
info "  ${DOWNLOAD_URL}"

HTTP_CODE="$(curl -fsSL \
  --output "${TMP_FILE}" \
  --write-out "%{http_code}" \
  "${DOWNLOAD_URL}" 2>&1)" || {
  err "Download failed (HTTP ${HTTP_CODE:-unknown})."
  err "URL: ${DOWNLOAD_URL}"
  exit 1
}

if [[ "${HTTP_CODE}" != "200" ]]; then
  err "Download failed with HTTP status: ${HTTP_CODE}"
  err "URL: ${DOWNLOAD_URL}"
  exit 1
fi

# ###########################################################################
# Integrity check — ensure the file is not empty/corrupt
# ###########################################################################
FILE_SIZE="$(wc -c < "${TMP_FILE}")"
MIN_SIZE=1000

if [[ "${FILE_SIZE}" -le "${MIN_SIZE}" ]]; then
  err "Downloaded file is suspiciously small (${FILE_SIZE} bytes ≤ ${MIN_SIZE} bytes)."
  err "The release asset may be missing or the download was truncated."
  exit 1
fi

info "Downloaded ${FILE_SIZE} bytes — integrity check passed."

# ###########################################################################
# Install binary
# ###########################################################################
INSTALL_DIR="/usr/local/bin"
INSTALL_PATH="${INSTALL_DIR}/aetherproxy"

chmod +x "${TMP_FILE}"

if [[ "${EUID}" -eq 0 ]]; then
  mv "${TMP_FILE}" "${INSTALL_PATH}"
else
  info "Root privileges required to install to ${INSTALL_DIR}. Invoking sudo…"
  sudo mv "${TMP_FILE}" "${INSTALL_PATH}"
fi

# Trap no longer needs to remove the file (it has been moved)
trap - EXIT

# ###########################################################################
# Success
# ###########################################################################
ok "${BOLD}AetherProxy ${TAG}${RESET}${GREEN} installed successfully!${RESET}"
ok "Binary location: ${INSTALL_PATH}"
echo
echo -e "  Run ${BOLD}aetherproxy --help${RESET} to get started."
