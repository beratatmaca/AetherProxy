#!/usr/bin/env bash
# tools/run-tests.sh — Local and CI test runner for AetherProxy.
#
# Usage:
#   bash tools/run-tests.sh [--asan]
#
# With no flags: expects ./build/aetherproxy to already exist (built externally
# or by the CI 'integration' job). Runs the Playwright suite.
#
# --asan  Build under AddressSanitizer, run the integration suite.
#
# Environment variables honoured:
#   BINARY   Path to binary (default: ./build/aetherproxy)
#   PORT     HTTP port for integration tests (default: 8095)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BINARY="${BINARY:-./build/aetherproxy}"
PORT="${PORT:-8095}"
MODE="integration"

for arg in "$@"; do
  case "$arg" in
    --asan) MODE="asan" ;;
    *)      echo "Unknown flag: $arg" >&2; exit 1 ;;
  esac
done

# ################ build helpers ################

embed_assets() {
  echo "[run-tests] Embedding web assets…"
  bash tools/embed-assets.sh
}

build_preset() {
  local preset="$1"
  echo "[run-tests] Configuring preset: $preset"
  cmake --preset "$preset"
  echo "[run-tests] Building preset: $preset"
  cmake --build --preset "$preset" --parallel "$(nproc)"
}

# ################ ASan integration run ################

run_asan() {
  embed_assets
  build_preset debug

  echo "[run-tests] Running integration suite under ASan…"
  export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
  export LSAN_OPTIONS="suppressions=$REPO_ROOT/tools/lsan.supp"
  export BROWSERS="${BROWSERS:-chromium}"
  BINARY="build-debug/aetherproxy"
  export BINARY
  run_integration
}

# ################ Playwright integration suite ################

run_integration() {
  if [[ ! -x "$BINARY" ]]; then
    echo "[run-tests] Binary not found at '$BINARY'. Build first or set BINARY=<path>." >&2
    exit 1
  fi

  # Stale hosts from an aborted run poison the port. Sweep the test
  # port only. Never kill unrelated aetherproxy sessions.
  if command -v fuser >/dev/null 2>&1; then
    fuser -k -TERM "${PORT}/tcp" 2>/dev/null || true
    sleep 0.5
  fi

  # Ensure Playwright browsers are installed.
  if ! (cd tests && npx playwright --version &>/dev/null); then
    echo "[run-tests] Installing Playwright…"
    (cd tests && npm ci && npx playwright install --with-deps chromium firefox)
  fi

  echo "[run-tests] Running Playwright integration suite…"
  PORT="$PORT" node tests/integration.js
}

# ################ dispatch ################

case "$MODE" in
  asan)        run_asan ;;
  integration) run_integration ;;
esac
