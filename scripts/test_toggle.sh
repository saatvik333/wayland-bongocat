#!/usr/bin/env bash
set -euo pipefail

binary=$(realpath ./build/bongocat)
if [[ ! -x "$binary" || -z "${WAYLAND_DISPLAY:-}" || -z "${XDG_RUNTIME_DIR:-}" ]]; then
  echo "Skipping: build the application and provide a Wayland session."
  exit 0
fi
socket=$WAYLAND_DISPLAY
[[ "$socket" = /* ]] || socket="$XDG_RUNTIME_DIR/$socket"
if [[ ! -S "$socket" ]]; then
  echo "Skipping: Wayland socket unavailable."
  exit 0
fi
runtime=$(mktemp -d)
child=
cleanup() {
  if [[ -n "$child" ]]; then
    kill -TERM "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  rm -rf "$runtime"
}
trap cleanup EXIT
ln -s "$socket" "$runtime/wayland-test"
export XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=wayland-test
printf 'overlay_opacity=0\n' > "$runtime/test.conf"
for cycle in 1 2; do
  "$binary" --toggle -c "$runtime/test.conf" > "$runtime/log" 2>&1 &
  child=$!
  ready=0
  for _attempt in {1..50}; do
    if "$binary" --status >/dev/null 2>&1; then ready=1; break; fi
    kill -0 "$child" 2>/dev/null || break
    sleep 0.05
  done
  if [[ "$ready" != 1 ]]; then cat "$runtime/log"; exit 1; fi
  "$binary" --toggle
  wait "$child"
  child=
  echo "Toggle cycle $cycle passed."
done
