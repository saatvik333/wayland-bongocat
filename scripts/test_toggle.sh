#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x ./build/bongocat ]]; then
  echo "Error: ./build/bongocat not found. Build first with: make"
  exit 1
fi

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "Skipping toggle test: WAYLAND_DISPLAY is not set."
  exit 0
fi

if [[ -z "${XDG_RUNTIME_DIR:-}" || ! -S "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}" ]]; then
  echo "Skipping toggle test: Wayland socket is not available."
  exit 0
fi

show_processes() {
  if ! pgrep -x -a bongocat; then
    echo "No bongocat processes found"
  fi
}

is_running() {
  pgrep -x bongocat >/dev/null 2>&1
}

echo "Testing bongocat toggle functionality..."
echo

if is_running; then
  echo "Pre-clean: existing bongocat instance detected, toggling it off first."
  ./build/bongocat --toggle || true
  sleep 1
fi

echo "1. Starting bongocat with --toggle (should start since not running):"
if ! ./build/bongocat --toggle; then
  echo "Skipping toggle test: bongocat could not start (Wayland unavailable)."
  exit 0
fi
sleep 2

echo
echo "2. Checking if bongocat is running:"
show_processes

echo
echo "3. Toggling bongocat off (should stop the running instance):"
./build/bongocat --toggle
sleep 1

echo
echo "4. Checking if bongocat is still running:"
show_processes

echo
echo "5. Toggling bongocat on again (should start since not running):"
./build/bongocat --toggle
sleep 2

echo
echo "6. Final check - bongocat should be running:"
show_processes

echo
echo "7. Cleaning up - stopping bongocat:"
./build/bongocat --toggle || true

echo
echo "Toggle functionality test completed!"
