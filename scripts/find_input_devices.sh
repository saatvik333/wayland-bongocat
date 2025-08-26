#!/usr/bin/bash

# Wayland Bongo Cat - Input Device Discovery Tool
# Professional input device finder with comprehensive analysis
# Version: 1.2.5

set -euo pipefail

# Color setup - detect if colors are supported
if [[ -t 1 ]] && [[ "${TERM:-}" != "dumb" ]] && [[ "${NO_COLOR:-}" != "1" ]]; then
    readonly RED='\033[0;31m'
    readonly GREEN='\033[0;32m'
    readonly YELLOW='\033[1;33m'
    readonly BLUE='\033[0;34m'
    readonly PURPLE='\033[0;35m'
    readonly CYAN='\033[0;36m'
    readonly WHITE='\033[1;37m'
    readonly NC='\033[0m' # No Color
    readonly USE_COLORS=true
else
    # No colors - use empty strings
    readonly RED=''
    readonly GREEN=''
    readonly YELLOW=''
    readonly BLUE=''
    readonly PURPLE=''
    readonly CYAN=''
    readonly WHITE=''
    readonly NC=''
    readonly USE_COLORS=false
fi

# Helper function for colored output
print_colored() {
    local color="$1"
    local text="$2"
    if [[ "$USE_COLORS" == "true" ]]; then
        echo -e "${color}${text}${NC}"
    else
        echo "$text"
    fi
}

# Status symbols
readonly CHECK="[OK]"
readonly CROSS="[ERROR]"
readonly WARNING="[WARN]"
readonly INFO="[INFO]"
readonly SEARCH="[SCAN]"
readonly KEYBOARD="[DEVICES]"
readonly CONFIG="[CONFIG]"
readonly TEST="[TEST]"

# Script metadata
readonly SCRIPT_NAME="bongocat-find-devices"
readonly VERSION="1.2.5"

# Command line options
SHOW_ALL=false
GENERATE_CONFIG=false
TEST_DEVICES=false
VERBOSE=false

# Usage information
usage() {
    if [[ "$USE_COLORS" == "true" ]]; then
        printf "${WHITE}%s v%s${NC}\n" "$SCRIPT_NAME" "$VERSION"
        printf "Professional input device discovery for Wayland Bongo Cat\n\n"
        printf "${WHITE}USAGE:${NC}\n"
        printf "    %s [OPTIONS]\n\n" "$0"
        printf "${WHITE}OPTIONS:${NC}\n"
        printf "    -a, --all              Show all input devices (including mice, touchpads)\n"
        printf "    -g, --generate-config  Generate configuration file to stdout\n"
        printf "    -t, --test            Test device responsiveness (requires root)\n"
        printf "    -v, --verbose         Show detailed device information\n"
        printf "    -h, --help            Show this help message\n\n"
        printf "${WHITE}EXAMPLES:${NC}\n"
        printf "    %s                     # Basic device discovery\n" "$0"
        printf "    %s --all --verbose     # Comprehensive device analysis\n" "$0"
        printf "    %s --generate-config > bongocat.conf  # Generate config file\n\n" "$0"
        printf "${WHITE}DESCRIPTION:${NC}\n"
        printf "    This tool scans your system for input devices and provides configuration\n"
        printf "    suggestions for Wayland Bongo Cat. It identifies keyboards, checks\n"
        printf "    permissions, and generates ready-to-use configuration snippets.\n\n"
        printf "${WHITE}MONITOR DETECTION:${NC}\n"
        printf "    For multi-monitor setups, use these commands to find monitor names:\n"
        printf "    • wlr-randr                    # List all monitors (recommended)\n"
        printf "    • swaymsg -t get_outputs       # Sway compositor only\n"
        printf "    • bongocat logs show detected monitors during startup\n\n"
    else
        cat << EOF
${SCRIPT_NAME} v${VERSION}
Professional input device discovery for Wayland Bongo Cat

USAGE:
    $0 [OPTIONS]

OPTIONS:
    -a, --all              Show all input devices (including mice, touchpads)
    -g, --generate-config  Generate configuration file to stdout
    -t, --test            Test device responsiveness (requires root)
    -v, --verbose         Show detailed device information
    -h, --help            Show this help message

EXAMPLES:
    $0                     # Basic device discovery
    $0 --all --verbose     # Comprehensive device analysis
    $0 --generate-config > bongocat.conf  # Generate config file

DESCRIPTION:
    This tool scans your system for input devices and provides configuration
    suggestions for Wayland Bongo Cat. It identifies keyboards, checks
    permissions, and generates ready-to-use configuration snippets.

MONITOR DETECTION:
    For multi-monitor setups, use these commands to find monitor names:
    • wlr-randr                    # List all monitors (recommended)
    • swaymsg -t get_outputs       # Sway compositor only
    • bongocat logs show detected monitors during startup

EOF
    fi
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -a|--all)
                SHOW_ALL=true
                shift
                ;;
            -g|--generate-config)
                GENERATE_CONFIG=true
                shift
                ;;
            -t|--test)
                TEST_DEVICES=true
                shift
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                if [[ "$USE_COLORS" == "true" ]]; then
                    echo -e "${RED}Error: Unknown option '$1'${NC}" >&2
                else
                    echo "Error: Unknown option '$1'" >&2
                fi
                echo "Use --help for usage information." >&2
                exit 1
                ;;
        esac
    done
}

# Print header
print_header() {
    if [[ "$GENERATE_CONFIG" == "false" ]]; then
        if [[ "$USE_COLORS" == "true" ]]; then
            echo -e "${PURPLE}╔══════════════════════════════════════════════════════════════════╗${NC}"
            echo -e "${PURPLE}║${NC} ${WHITE}Wayland Bongo Cat - Input Device Discovery v${VERSION}${NC}                ${PURPLE}║${NC}"
            echo -e "${PURPLE}╚══════════════════════════════════════════════════════════════════╝${NC}"
        else
            echo "=================================================================="
            echo " Wayland Bongo Cat - Input Device Discovery v${VERSION}"
            echo "=================================================================="
        fi
        echo
    fi
}

# Check if running as root
check_permissions() {
    if [[ $EUID -eq 0 ]]; then
        return 0  # Running as root
    else
        return 1  # Not running as root
    fi
}

# Check if user is in input group
check_input_group() {
    if groups | grep -q '\binput\b'; then
        return 0  # User is in input group
    else
        return 1  # User is not in input group
    fi
}

# Get device accessibility status
get_device_status() {
    local device="$1"

    if [[ -r "$device" ]]; then
        echo -e "${GREEN}${CHECK} Accessible${NC}"
        return 0
    else
        echo -e "${RED}${CROSS} Permission Denied${NC}"
        return 1
    fi
}

# Get device type based on name, capabilities, and handlers
get_device_type() {
    local device_name="$1"
    local capabilities="$2"
    local handlers="$3"

    # Convert to lowercase for matching
    local name_lower=$(echo "$device_name" | tr '[:upper:]' '[:lower:]')
    local handlers_lower=$(echo "$handlers" | tr '[:upper:]' '[:lower:]')

    # Check for keyboard indicators
    # Look for "kbd" handler (most reliable), keyboard in name, or keyboard-like capabilities
    if [[ "$handlers_lower" =~ kbd ]] || [[ "$name_lower" =~ keyboard ]] || [[ "$capabilities" =~ "120013" ]] || [[ "$capabilities" =~ "12001f" ]]; then
        # Determine keyboard type
        if [[ "$name_lower" =~ (bluetooth|wireless|bt) ]] || [[ "$handlers_lower" =~ bluetooth ]]; then
            echo "Keyboard (Bluetooth)"
        elif [[ "$name_lower" =~ (usb|external) ]]; then
            echo "Keyboard (USB)"
        else
            # Check if it's likely a Bluetooth keyboard based on common brands
            if [[ "$name_lower" =~ (keychron|logitech|corsair|razer|steelseries|apple|microsoft) ]] && [[ ! "$name_lower" =~ (mouse|trackpad|touchpad) ]]; then
                echo "Keyboard (Bluetooth)"
            else
                echo "Keyboard"
            fi
        fi
    elif [[ "$name_lower" =~ mouse ]] || [[ "$capabilities" =~ "110000" ]]; then
        echo "Mouse"
    elif [[ "$name_lower" =~ (touchpad|trackpad|synaptics) ]]; then
        echo "Touchpad"
    elif [[ "$name_lower" =~ (touchscreen|touch) ]]; then
        echo "Touchscreen"
    else
        echo "Input Device"
    fi
}

# Parse device information from /proc/bus/input/devices
parse_devices() {
    local show_all="$1"
    local devices=()

    if [[ ! -r /proc/bus/input/devices ]]; then
        echo -e "${RED}${CROSS} Cannot read /proc/bus/input/devices${NC}" >&2
        echo -e "${INFO} Try running with sudo for full device information" >&2
        return 1
    fi

    # Parse the devices file
    local current_name=""
    local current_handlers=""
    local current_capabilities=""

    while IFS= read -r line; do
        if [[ "$line" =~ ^I: ]]; then
            # Reset for new device
            current_name=""
            current_handlers=""
            current_capabilities=""
        elif [[ "$line" =~ ^N:\ Name=\"(.*)\" ]]; then
            current_name="${BASH_REMATCH[1]}"
        elif [[ "$line" =~ ^H:\ Handlers=(.*) ]]; then
            current_handlers="${BASH_REMATCH[1]}"
        elif [[ "$line" =~ ^B:\ EV=(.*) ]]; then
            current_capabilities="${BASH_REMATCH[1]}"
        elif [[ "$line" =~ ^$ ]] && [[ -n "$current_name" ]]; then
            # End of device block, process it
            local device_type=$(get_device_type "$current_name" "$current_capabilities" "$current_handlers")

            # Extract event handlers
            local event_handlers=($(echo "$current_handlers" | grep -o 'event[0-9]\+' || true))

            # Filter devices based on show_all flag
            if [[ "$show_all" == "true" ]] || [[ "$device_type" =~ Keyboard ]]; then
                for handler in "${event_handlers[@]}"; do
                    local device_path="/dev/input/$handler"
                    if [[ -e "$device_path" ]]; then
                        devices+=("$current_name|$device_path|$device_type")
                    fi
                done
            fi
        fi
    done < /proc/bus/input/devices

    # Handle last device if file doesn't end with empty line
    if [[ -n "$current_name" ]]; then
        local device_type=$(get_device_type "$current_name" "$current_capabilities" "$current_handlers")
        local event_handlers=($(echo "$current_handlers" | grep -o 'event[0-9]\+' || true))

        if [[ "$show_all" == "true" ]] || [[ "$device_type" =~ Keyboard ]]; then
            for handler in "${event_handlers[@]}"; do
                local device_path="/dev/input/$handler"
                if [[ -e "$device_path" ]]; then
                    devices+=("$current_name|$device_path|$device_type")
                fi
            done
        fi
    fi

    printf '%s\n' "${devices[@]}"
}

# Display device information
display_devices() {
    local show_all="$1"
    local devices

    if [[ "$GENERATE_CONFIG" == "false" ]]; then
        if [[ "$USE_COLORS" == "true" ]]; then
            echo -e "${SEARCH} ${WHITE}Scanning for input devices...${NC}"
        else
            echo "${SEARCH} Scanning for input devices..."
        fi
        echo
    fi

    # Get device list
    if ! devices=$(parse_devices "$show_all"); then
        return 1
    fi

    if [[ -z "$devices" ]]; then
        if [[ "$GENERATE_CONFIG" == "false" ]]; then
            echo -e "${WARNING} ${YELLOW}No input devices found${NC}"
            echo -e "${INFO} Try running with sudo: ${WHITE}sudo $0${NC}"
        fi
        return 1
    fi

    local keyboard_devices=()
    local accessible_keyboards=()

    if [[ "$GENERATE_CONFIG" == "false" ]]; then
        echo -e "${KEYBOARD} ${WHITE}Found Input Devices:${NC}"
    fi

    # Process and display each device
    while IFS='|' read -r name path type; do
        if [[ "$GENERATE_CONFIG" == "false" ]]; then
            echo -e "${BLUE}┌─────────────────────────────────────────────────────────────────┐${NC}"
            echo -e "${BLUE}│${NC} ${WHITE}Device:${NC} $(printf "%-50s" "$name") ${BLUE}     │${NC}"
            echo -e "${BLUE}│${NC} ${WHITE}Path:${NC}   $(printf "%-50s" "$path") ${BLUE}     │${NC}"
            echo -e "${BLUE}│${NC} ${WHITE}Type:${NC}   $(printf "%-50s" "$type") ${BLUE}     │${NC}"

            local status
            status=$(get_device_status "$path")
            echo -e "${BLUE}│${NC} ${WHITE}Status:${NC} $status $(printf "%*s" $((50 - ${#status} + 10)) "") ${BLUE}     │${NC}"
            echo -e "${BLUE}└─────────────────────────────────────────────────────────────────┘${NC}"
            echo
        fi

        # Collect keyboard devices for configuration
        if [[ "$type" =~ Keyboard ]]; then
            keyboard_devices+=("$path|$name")
            if [[ -r "$path" ]]; then
                accessible_keyboards+=("$path|$name")
            fi
        fi
    done <<< "$devices"

    # Generate configuration suggestions
    if [[ "${#keyboard_devices[@]}" -gt 0 ]]; then
        generate_config_suggestions "${keyboard_devices[@]}"
    else
        if [[ "$GENERATE_CONFIG" == "false" ]]; then
            echo -e "${WARNING} ${YELLOW}No keyboard devices found${NC}"
        fi
    fi

    # Show permission help if needed
    if [[ "${#accessible_keyboards[@]}" -lt "${#keyboard_devices[@]}" ]] && [[ "$GENERATE_CONFIG" == "false" ]]; then
        show_permission_help
    fi
}

# Generate configuration suggestions
generate_config_suggestions() {
    local devices=("$@")

    if [[ "$GENERATE_CONFIG" == "true" ]]; then
        # Generate full config file
        cat << 'EOF'
# Wayland Bongo Cat Configuration
# Generated by bongocat-find-devices

# Visual settings
cat_height=50                    # Size of bongo cat (16-128)
cat_x_offset=0                   # Horizontal position offset
cat_y_offset=0                   # Vertical position offset
overlay_opacity=150              # Background opacity (0-255)

# Animation settings
fps=60                           # Frame rate (1-120)
keypress_duration=100            # Animation duration (ms)
test_animation_interval=3        # Test animation every N seconds (0=off)

# Input devices
EOF
        for device_info in "${devices[@]}"; do
            IFS='|' read -r path name <<< "$device_info"
            echo "keyboard_device=$path   # $name"
        done
        cat << 'EOF'

# Debug
enable_debug=1                   # Show debug messages
EOF
    else
        echo -e "${CONFIG} ${WHITE}Configuration Suggestions:${NC}"
        echo -e "${WHITE}Add these lines to your bongocat.conf:${NC}"
        echo

        for device_info in "${devices[@]}"; do
            IFS='|' read -r path name <<< "$device_info"
            echo -e "${CYAN}keyboard_device=$path${NC}   ${WHITE}# $name${NC}"
        done
        echo
    fi
}

# Show permission help
show_permission_help() {
    echo -e "${WARNING} ${WHITE}Permission Issues Detected:${NC}"
    echo -e "${INFO} Some devices are not accessible. To fix this:"
    echo
    echo -e "${WHITE}1. Add your user to the input group:${NC}"
    echo -e "   ${CYAN}sudo usermod -a -G input \$USER${NC}"
    echo -e "   ${YELLOW}(Log out and back in for changes to take effect)${NC}"
    echo
    echo -e "${WHITE}2. Or create a udev rule:${NC}"
    echo -e "   ${CYAN}echo 'KERNEL==\"event*\", GROUP=\"input\", MODE=\"0664\"' | sudo tee /etc/udev/rules.d/99-input.rules${NC}"
    echo -e "   ${CYAN}sudo udevadm control --reload-rules${NC}"
    echo
}

# Test device responsiveness
test_devices() {
    if ! command -v evtest >/dev/null 2>&1; then
        echo -e "${RED}${CROSS} evtest not found${NC}" >&2
        echo -e "${INFO} Install with: ${WHITE}sudo apt install evtest${NC} (Ubuntu/Debian)" >&2
        echo -e "${INFO} Or: ${WHITE}sudo pacman -S evtest${NC} (Arch Linux)" >&2
        return 1
    fi

    if ! check_permissions; then
        echo -e "${RED}${CROSS} Root privileges required for device testing${NC}" >&2
        echo -e "${INFO} Run with: ${WHITE}sudo $0 --test${NC}" >&2
        return 1
    fi

    echo -e "${TEST} ${WHITE}Device Testing Mode${NC}"
    echo -e "${INFO} This will launch evtest for device testing"
    echo -e "${INFO} Press Ctrl+C to exit evtest when done"
    echo

    exec evtest
}

# Main function
main() {
    parse_args "$@"

    if [[ "$TEST_DEVICES" == "true" ]]; then
        test_devices
        return $?
    fi

    print_header
    display_devices "$SHOW_ALL"
}

# Run main function with all arguments
main "$@"
