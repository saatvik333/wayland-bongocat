# Bongo Cat Wayland Overlay

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)
[![Version](https://img.shields.io/badge/version-1.3.0-blue.svg)](https://github.com/saatvik333/wayland-bongocat/releases)

A cute Wayland overlay that shows an animated bongo cat reacting to your keyboard input.

![Demo](assets/demo.gif)

## Features

- 🎯 Real-time keyboard animation
- 🔥 Hot-reload configuration
- 🎮 Auto-hides in fullscreen apps
- 🖥️ Multi-monitor support
- 😴 Idle/scheduled sleep mode
- ⚡ Lightweight (~20MB RAM)

## Quick Start

### Install

```bash
# Arch Linux
yay -S bongocat

# Other distros - build from source
git clone https://github.com/saatvik333/wayland-bongocat.git
cd wayland-bongocat && make
```

### Setup Permissions

```bash
sudo usermod -a -G input $USER
# Log out and back in
```

### Find Your Keyboard

```bash
bongocat-find-devices  # or ./scripts/find_input_devices.sh
```

### Run

```bash
bongocat --watch-config
```

## Configuration

Create `~/.config/bongocat/bongocat.conf`:

```ini
# ═══════════════════════════════════════════════════════════════════════════
# BONGO CAT CONFIG - Minimal defaults, uncomment to customize
# ═══════════════════════════════════════════════════════════════════════════

# Position & Size
cat_height=80
cat_align=center
# cat_x_offset=0
# cat_y_offset=0

# Appearance
enable_antialiasing=1
overlay_height=80
overlay_opacity=0
overlay_position=bottom
# mirror_x=0
# mirror_y=0

# Input device (run bongocat-find-devices to find yours)
keyboard_device=/dev/input/event4

# Multi-monitor (optional - auto-detects by default)
# monitor=eDP-1

# Sleep mode (optional)
# idle_sleep_timeout=300
# enable_scheduled_sleep=0
# sleep_begin=22:00
# sleep_end=06:00
```

### Options Reference

<details>
<summary>Click to expand all options</summary>

| Option                | Values            | Default | Description          |
| --------------------- | ----------------- | ------- | -------------------- |
| `cat_height`          | 10-200            | 80      | Cat size in pixels   |
| `cat_align`           | left/center/right | center  | Horizontal alignment |
| `cat_x_offset`        | any int           | 0       | Horizontal offset    |
| `cat_y_offset`        | any int           | 0       | Vertical offset      |
| `enable_antialiasing` | 0/1               | 1       | Smooth scaling       |
| `overlay_height`      | 20-300            | 80      | Bar height           |
| `overlay_opacity`     | 0-255             | 0       | Background opacity   |
| `overlay_position`    | top/bottom        | bottom  | Screen position      |
| `layer`               | top/overlay       | top     | Layer type           |
| `keyboard_device`     | path              | —       | Device to monitor    |
| `monitor`             | name              | auto    | Target monitor       |
| `fps`                 | 1-120             | 60      | Frame rate           |
| `idle_sleep_timeout`  | seconds           | 0       | Sleep after idle     |
| `mirror_x`            | 0/1               | 0       | Flip horizontal      |
| `mirror_y`            | 0/1               | 0       | Flip vertical        |

</details>

## Command Line

```bash
bongocat [OPTIONS]

  -c, --config FILE    Config file path
  -w, --watch-config   Auto-reload on config change
  -t, --toggle         Start/stop toggle
  -h, --help           Help
  -v, --version        Version
```

## Troubleshooting

<details>
<summary>Permission denied on input device</summary>

```bash
sudo usermod -a -G input $USER
# Then log out and back in
```

</details>

<details>
<summary>Cat not responding to keyboard</summary>

1. Run `bongocat-find-devices` to find correct device
2. Update `keyboard_device` in config
3. Restart bongocat

</details>

<details>
<summary>Not showing on correct monitor</summary>

Add `monitor=YOUR_MONITOR` to config. Find monitor names with `wlr-randr` or `hyprctl monitors`.

</details>

## Building

```bash
git clone https://github.com/saatvik333/wayland-bongocat.git
cd wayland-bongocat
make          # Release build
make debug    # Debug build
```

**Requirements:** wayland-client, wayland-protocols, gcc/clang, make

## License

MIT License - see [LICENSE](LICENSE)
