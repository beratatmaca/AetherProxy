#!/usr/bin/env bash
# tools/run-tests.sh — Local and CI test runner for AetherProxy.
#
# Usage:
#   bash tools/run-tests.sh [--asan] [--tsan]
#
# With no flags: expects ./build/aetherproxy to already exist (built externally
# or by the CI 'integration' job). Runs the Playwright suite.
#
# --asan  Build under AddressSanitizer first, run ASan smoke test.
# --tsan  Build under ThreadSanitizer first, run TSan smoke test.
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
    --tsan) MODE="tsan" ;;
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

# ################ ASan smoke test ################

run_asan() {
  embed_assets
  build_preset debug

  echo "[run-tests] Running ASan smoke test…"
  local out tmphome
  tmphome=$(mktemp -d)
  HOME="$tmphome" build-debug/aetherproxy config set port "$((PORT + 1))" > /dev/null
  HOME="$tmphome" build-debug/aetherproxy config set offline true > /dev/null
  out=$(ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 HOME="$tmphome" \
        timeout 4 build-debug/aetherproxy 2>&1 || true)
  echo "$out"

  if echo "$out" | grep -qE '[a-z]+-[a-z]+-[a-z]+-[0-9]{5}'; then
    echo "[run-tests] ✓ ASan smoke test passed — no heap errors detected."
  else
    echo "[run-tests] ✗ ASan binary did not emit a room code (possible ASan abort)." >&2
    exit 1
  fi
}

# ################ TSan smoke test ################

run_tsan() {
  embed_assets
  build_preset tsan

  echo "[run-tests] Running TSan smoke test…"
  local out tmphome
  tmphome=$(mktemp -d)
  HOME="$tmphome" build-tsan/aetherproxy config set port "$((PORT + 2))" > /dev/null
  HOME="$tmphome" build-tsan/aetherproxy config set offline true > /dev/null
  out=$(TSAN_OPTIONS=halt_on_error=1 HOME="$tmphome" \
        timeout 4 build-tsan/aetherproxy 2>&1 || true)
  echo "$out"

  if echo "$out" | grep -qE '[a-z]+-[a-z]+-[a-z]+-[0-9]{5}'; then
    echo "[run-tests] ✓ TSan smoke test passed — no data races detected."
  else
    echo "[run-tests] ✗ TSan binary did not emit a room code (possible TSan abort)." >&2
    exit 1
  fi
}

# ################ Playwright integration suite ################

run_integration() {
  if [[ ! -x "$BINARY" ]]; then
    echo "[run-tests] Binary not found at '$BINARY'. Build first or set BINARY=<path>." >&2
    exit 1
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
  tsan)        run_tsan ;;
  integration) run_integration ;;
esac
