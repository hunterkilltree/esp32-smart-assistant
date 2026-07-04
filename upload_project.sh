#!/usr/bin/env bash
#
# upload_project.sh — build and flash this PlatformIO project to the board.
#
# Usage:
#   ./upload_project.sh [options]
#
# Options:
#   -e, --env <name>     PlatformIO environment (default: first [env:...] in platformio.ini)
#   -p, --port <port>    Upload/monitor port (e.g. COM5, /dev/ttyUSB0). Default: pio auto-detects.
#   -m, --monitor        Open the serial monitor after a successful upload.
#   -b, --build-only     Only build, don't upload.
#   -c, --clean          Run a clean build first (pio run -t clean).
#   --pincheck           Shortcut for -e esp32-s3-eye-pincheck -m (flash the diagnostic
#                         pin-check firmware and immediately open the monitor to watch it).
#   -h, --help            Show this help.
#
# Examples:
#   ./upload_project.sh                  # build + upload, auto-detect port
#   ./upload_project.sh -p COM3 -m       # upload to COM5, then open the monitor
#   ./upload_project.sh -b               # just build, don't touch the board
#   ./upload_project.sh --pincheck -p COM5   # flash + watch the pin-check firmware

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ENVIRONMENT=""
PORT=""
MONITOR=false
BUILD_ONLY=false
CLEAN=false

usage() {
  sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -e|--env) ENVIRONMENT="$2"; shift 2 ;;
    -p|--port) PORT="$2"; shift 2 ;;
    -m|--monitor) MONITOR=true; shift ;;
    -b|--build-only) BUILD_ONLY=true; shift ;;
    -c|--clean) CLEAN=true; shift ;;
    --pincheck) ENVIRONMENT="esp32-s3-eye-pincheck"; MONITOR=true; shift ;;
    -h|--help) usage 0 ;;
    *) echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

# --- locate the PlatformIO CLI ---
find_pio() {
  if command -v pio >/dev/null 2>&1; then
    command -v pio
    return
  fi
  if command -v platformio >/dev/null 2>&1; then
    command -v platformio
    return
  fi

  local candidates=(
    "${HOME:-}/.platformio/penv/Scripts/pio.exe"
    "${HOME:-}/.platformio/penv/bin/pio"
    "${USERPROFILE:-}/.platformio/penv/Scripts/pio.exe"
  )
  local c
  for c in "${candidates[@]}"; do
    if [[ -n "$c" && -x "$c" ]]; then
      echo "$c"
      return
    fi
  done
}

PIO="$(find_pio)"
if [[ -z "$PIO" ]]; then
  echo "error: could not find the PlatformIO CLI (pio)." >&2
  echo "       Tried PATH and the default PlatformIO IDE install location" >&2
  echo "       (~/.platformio/penv/...). Install PlatformIO or add it to PATH." >&2
  exit 1
fi
echo "Using PlatformIO CLI: $PIO"

# --- default environment from platformio.ini if not given ---
if [[ -z "$ENVIRONMENT" ]]; then
  ENVIRONMENT="$(grep -m1 -oE '^\[env:[^]]+\]' platformio.ini | sed -E 's/^\[env:(.*)\]$/\1/')"
  if [[ -z "$ENVIRONMENT" ]]; then
    echo "error: could not determine environment from platformio.ini; pass -e <env>." >&2
    exit 1
  fi
fi
echo "Environment: $ENVIRONMENT"

# --- sanity check secrets.h exists (build fails without it, config.h includes it) ---
if [[ ! -f "include/secrets.h" ]]; then
  echo "warning: include/secrets.h not found — copying from secrets.h.example." >&2
  cp include/secrets.h.example include/secrets.h
  echo "         Edit include/secrets.h with real WiFi/WS credentials before this build is useful." >&2
fi

if $CLEAN; then
  echo "Cleaning previous build..."
  "$PIO" run -e "$ENVIRONMENT" -t clean
fi

PIO_ARGS=(run -e "$ENVIRONMENT")
if ! $BUILD_ONLY; then
  PIO_ARGS+=(-t upload)
  if [[ -n "$PORT" ]]; then
    PIO_ARGS+=(--upload-port "$PORT")
  fi
fi

echo "Running: $PIO ${PIO_ARGS[*]}"
"$PIO" "${PIO_ARGS[@]}"

if $MONITOR; then
  MONITOR_ARGS=(device monitor -e "$ENVIRONMENT")
  if [[ -n "$PORT" ]]; then
    MONITOR_ARGS+=(--port "$PORT")
  fi
  echo "Opening serial monitor (Ctrl+C to exit)..."
  "$PIO" "${MONITOR_ARGS[@]}"
fi
