#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════════
# Bongo Cat - Input Device Discovery Tool v1.3.2
# Interactive keyboard detection by listening for actual key events
# ═══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

VERSION="1.3.2"
SCRIPT_NAME="bongocat-find-devices"

# Colors
if [[ -t 1 ]] && [[ "${NO_COLOR:-}" != "1" ]]; then
  RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m'
  BLUE='\033[0;34m' CYAN='\033[0;36m' BOLD='\033[1m' DIM='\033[2m' NC='\033[0m'
else
  RED='' GREEN='' YELLOW='' BLUE='' CYAN='' BOLD='' DIM='' NC=''
fi

# ─────────────────────────────────────────────────────────────────────────────
# Helper Functions
# ─────────────────────────────────────────────────────────────────────────────

info()    { echo -e "${BLUE}→${NC} $*"; }
success() { echo -e "${GREEN}✓${NC} $*"; }
warn()    { echo -e "${YELLOW}!${NC} $*"; }
error()   { echo -e "${RED}✗${NC} $*" >&2; }

header() {
  echo
  echo -e "${BOLD}$*${NC}"
  echo -e "${BLUE}$(printf '─%.0s' {1..60})${NC}"
}

# ─────────────────────────────────────────────────────────────────────────────
# Device Discovery
# ─────────────────────────────────────────────────────────────────────────────

# Get all event devices with kbd handler (potential keyboards)
get_kbd_devices() {
  local devices=()

  if [[ ! -r /proc/bus/input/devices ]]; then
    return 1
  fi

  local name="" handlers=""

  while IFS= read -r line; do
    case "$line" in
      N:\ Name=\"*\")
        name="${line#N: Name=\"}"
        name="${name%\"}"
        ;;
      H:\ Handlers=*)
        handlers="${line#H: Handlers=}"
        ;;
      "")
        if [[ "$handlers" =~ kbd ]] && [[ "$handlers" =~ event ]]; then
          local event
          event=$(echo "$handlers" | grep -o 'event[0-9]*' | head -1)
          if [[ -n "$event" ]] && [[ -e "/dev/input/$event" ]]; then
            devices+=("$event|$name")
          fi
        fi
        name="" handlers=""
        ;;
    esac
  done < /proc/bus/input/devices

  printf '%s\n' "${devices[@]}"
}

# Check if device is readable
check_device() {
  local path="$1"
  [[ -r "$path" ]]
}

# ─────────────────────────────────────────────────────────────────────────────
# Interactive Detection
# ─────────────────────────────────────────────────────────────────────────────

# Listen for key events on a device (runs in background)
listen_device() {
  local device="$1"
  local output_file="$2"
  local timeout="$3"

  # Use timeout + cat to read raw events
  # Key events are detected by the presence of specific byte patterns
  # Type 1 (EV_KEY) events indicate keyboard activity
  timeout "$timeout" cat "/dev/input/$device" 2>/dev/null | head -c 1000 > "$output_file" &
  echo $!
}

# Detect keyboards interactively
interactive_detect() {
  local timeout="${1:-5}"
  local devices
  devices=$(get_kbd_devices) || { error "Cannot read device list"; return 1; }

  if [[ -z "$devices" ]]; then
    error "No input devices with kbd handler found"
    info "Try: sudo $SCRIPT_NAME --interactive"
    return 1
  fi

  # Check permissions
  local has_permission=false
  while IFS='|' read -r event name; do
    if check_device "/dev/input/$event"; then
      has_permission=true
      break
    fi
  done <<< "$devices"

  if [[ "$has_permission" == "false" ]]; then
    error "Cannot read input devices (permission denied)"
    echo
    info "Fix with: ${CYAN}sudo usermod -a -G input \$USER${NC}"
    info "Then log out and back in"
    echo
    info "Or run: ${CYAN}sudo $SCRIPT_NAME --interactive${NC}"
    return 1
  fi

  # Create temp directory for output files
  local tmpdir
  tmpdir=$(mktemp -d)
  trap "rm -rf '$tmpdir'" EXIT

  header "Interactive Keyboard Detection"
  echo
  echo -e "  ${BOLD}Press keys on ALL your keyboards for ${timeout} seconds...${NC}"
  echo -e "  ${DIM}(Internal laptop keyboard, external keyboards, etc.)${NC}"
  echo

  # Start listening on all accessible devices
  local pids=()
  local device_list=()

  while IFS='|' read -r event name; do
    if check_device "/dev/input/$event"; then
      local outfile="$tmpdir/$event"
      local pid
      pid=$(listen_device "$event" "$outfile" "$timeout")
      pids+=("$pid")
      device_list+=("$event|$name|$outfile")
    fi
  done <<< "$devices"

  # Show countdown
  for ((i=timeout; i>0; i--)); do
    echo -ne "\r  ${CYAN}Listening... ${i}s remaining ${NC}  "
    sleep 1
  done
  echo -e "\r  ${GREEN}✓ Detection complete!${NC}              "

  # Wait for all listeners to finish
  for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null || true
  done

  echo

  # Check which devices received input
  local detected_keyboards=()
  local other_devices=()

  for entry in "${device_list[@]}"; do
    IFS='|' read -r event name outfile <<< "$entry"

    if [[ -s "$outfile" ]]; then
      # Device received input - it's a keyboard!
      detected_keyboards+=("$event|$name")
    else
      other_devices+=("$event|$name")
    fi
  done

  # Show results
  header "Detection Results"

  if [[ ${#detected_keyboards[@]} -eq 0 ]]; then
    warn "No keyboards detected"
    echo
    info "Make sure you pressed keys during the detection window"
    info "Try again with: $SCRIPT_NAME --interactive"
    return 1
  fi

  echo -e "  ${GREEN}Detected keyboards:${NC}"
  for entry in "${detected_keyboards[@]}"; do
    IFS='|' read -r event name <<< "$entry"
    echo -e "    ${GREEN}✓${NC} ${BOLD}$name${NC}"
    echo -e "      ${CYAN}/dev/input/$event${NC}"
  done

  if [[ ${#other_devices[@]} -gt 0 ]]; then
    echo
    echo -e "  ${DIM}Other devices (no input detected):${NC}"
    for entry in "${other_devices[@]}"; do
      IFS='|' read -r event name <<< "$entry"
      echo -e "    ${DIM}○ $name (/dev/input/$event)${NC}"
    done
  fi

  # Config suggestion
  header "Add to Config"
  echo -e "  ${BOLD}~/.config/bongocat/bongocat.conf:${NC}"
  echo
  for entry in "${detected_keyboards[@]}"; do
    IFS='|' read -r event name <<< "$entry"
    echo -e "  ${CYAN}keyboard_device=/dev/input/$event${NC}  ${BOLD}# $name${NC}"
  done

  echo
}

# ─────────────────────────────────────────────────────────────────────────────
# Quick Mode (non-interactive, name-based)
# ─────────────────────────────────────────────────────────────────────────────

# Guess if device is keyboard by name
is_likely_keyboard() {
  local name="$1"
  local name_lower
  name_lower=$(echo "$name" | tr '[:upper:]' '[:lower:]')

  # Exclude obvious non-keyboards
  [[ "$name_lower" =~ (button|hotkey|speaker|video|consumer|system|avrcp|mouse|touchpad|trackpad) ]] && return 1

  # Include devices with "keyboard" in name
  [[ "$name_lower" =~ keyboard ]] && return 0

  # Include standard laptop keyboard
  [[ "$name_lower" =~ "at translated set 2" ]] && return 0

  return 1
}

quick_detect() {
  local devices
  devices=$(get_kbd_devices) || { error "Cannot read devices"; return 1; }

  if [[ -z "$devices" ]]; then
    warn "No input devices found"
    return 1
  fi

  echo
  echo -e "${BOLD}🐱 Bongo Cat Device Discovery${NC} v$VERSION"

  header "Detected Devices"

  local keyboards=()

  while IFS='|' read -r event name; do
    local status="ok"
    check_device "/dev/input/$event" || status="denied"

    if is_likely_keyboard "$name"; then
      echo -e "  ${GREEN}✓${NC} ${GREEN}[KEYBOARD]${NC} ${BOLD}$name${NC}"
      keyboards+=("$event|$name")
    else
      echo -e "  ${DIM}○ [other]    $name${NC}"
    fi
    echo -e "    ${CYAN}/dev/input/$event${NC}"
  done <<< "$devices"

  if [[ ${#keyboards[@]} -eq 0 ]]; then
    echo
    warn "Could not auto-detect keyboards by name"
    info "Use interactive mode: ${CYAN}$SCRIPT_NAME --interactive${NC}"
    return 1
  fi

  # Config suggestion
  header "Add to Config"
  echo -e "  ${BOLD}~/.config/bongocat/bongocat.conf:${NC}"
  echo
  for entry in "${keyboards[@]}"; do
    IFS='|' read -r event name <<< "$entry"
    echo -e "  ${CYAN}keyboard_device=/dev/input/$event${NC}  ${BOLD}# $name${NC}"
    echo -e "  ${CYAN}keyboard_name=$name${NC}"
  done

  echo
  echo -e "  keyboard_name support matching by substring."
  echo
  echo -e "  ${DIM}Not accurate? Use: $SCRIPT_NAME --interactive${NC}"
  echo
}

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

show_usage() {
  cat << EOF
${BOLD}$SCRIPT_NAME${NC} v$VERSION - Find keyboards for Bongo Cat

${BOLD}USAGE${NC}
    $SCRIPT_NAME [OPTIONS]

${BOLD}OPTIONS${NC}
    -i, --interactive   Detect keyboards by listening for key presses (recommended)
    -t, --timeout SEC   Detection timeout in seconds (default: 5)
    -g, --generate      Output config lines only (for piping)
    -h, --help          Show this help

${BOLD}EXAMPLES${NC}
    $SCRIPT_NAME                    # Quick detection (name-based)
    $SCRIPT_NAME -i                 # Interactive detection (recommended)
    $SCRIPT_NAME -i -t 10           # Interactive with 10 second timeout
EOF
}

main() {
  local mode="quick"
  local timeout=5
  local generate=false

  while [[ $# -gt 0 ]]; do
    case "$1" in
      -i|--interactive) mode="interactive"; shift ;;
      -t|--timeout) timeout="$2"; shift 2 ;;
      -g|--generate) generate=true; shift ;;
      -h|--help) show_usage; exit 0 ;;
      *) error "Unknown option: $1"; show_usage; exit 1 ;;
    esac
  done

  case "$mode" in
    interactive) interactive_detect "$timeout" ;;
    quick) quick_detect ;;
  esac
}

main "$@"
