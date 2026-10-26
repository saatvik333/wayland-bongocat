# Bongo Cat Wayland Overlay

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)
[![Version](https://img.shields.io/badge/version-2.1.1-blue.svg)](https://github.com/saatvik333/wayland-bongocat/releases)

A Wayland overlay with a bongo cat that plays along with your keyboard.

![Demo](assets/demo.gif)

- Reacts to every keyboard instantly, including keyboards plugged in later
- Follows the focused monitor, or stays on the monitors you name
- Hides under fullscreen windows
- Idle and scheduled sleep
- SVG artwork, sharp at any size and scale
- Live configuration reload

Requires a compositor with `wlr-layer-shell` (Sway, Hyprland, Niri, river,
Wayfire and others).

## Quick Start

```bash
# Arch Linux
yay -S bongocat

# From source
git clone https://github.com/saatvik333/wayland-bongocat.git
cd wayland-bongocat && make && sudo make install
```

Reading keyboards needs access to `/dev/input`:

```bash
sudo usermod -a -G input $USER   # then log out and back in
```

Run it:

```bash
bongocat --watch-config
```

Every keyboard is used by default. `bongocat --list-devices` shows each input
device, marks keyboards, and prints `keyboard_name=` and `keyboard_device=`
lines you can paste into the configuration to pick specific ones.
`bongocat --doctor` checks the configuration, device access and compositor
support at once.

## Configuration

The configuration is read from `$XDG_CONFIG_HOME/bongocat/bongocat.conf`,
`~/.config/bongocat/bongocat.conf` or `./bongocat.conf`, or from `-c FILE`.
Every setting is optional; [`bongocat.conf.example`](bongocat.conf.example)
documents each one. For example, a large cat at the bottom of the screen:

```ini
cat_height=110
overlay_height=120
overlay_opacity=0
overlay_position=bottom
cat_x_offset=0
cat_y_offset=0
```

| Setting                   | Values                        | Default   | Description                                   |
| ------------------------- | ----------------------------- | --------- | --------------------------------------------- |
| `cat_height`              | 10-200                        | 40        | Cat height in pixels                          |
| `cat_align`               | left/center/right             | center    | Horizontal alignment                          |
| `cat_x_offset`            | integer                       | 100       | Horizontal offset from the alignment          |
| `cat_y_offset`            | integer                       | 10        | Vertical offset from the bar center           |
| `overlay_height`          | 20-300                        | 50        | Bar height in pixels                          |
| `overlay_opacity`         | 0-255                         | 150       | Bar background opacity (0 is transparent)     |
| `overlay_position`        | top/bottom                    | top       | Screen edge                                   |
| `layer`                   | background/bottom/top/overlay | top       | Layer-shell layer                             |
| `mirror_x`, `mirror_y`    | 0/1                           | 0         | Flip the cat horizontally or vertically       |
| `disable_fullscreen_hide` | 0/1                           | 0         | Stay visible over fullscreen windows          |
| `monitor`                 | comma-separated names         | automatic | Monitors to show the cat on                   |
| `follow_active_monitor`   | 0/1                           | 1         | In automatic mode, follow the focused monitor |
| `keyboard_device`         | path in `/dev/input/`         | all       | Device to read; repeatable                    |
| `keyboard_name`           | text                          | all       | Read devices whose name contains it; repeatable |
| `fps`                     | 1-120                         | 60        | Maximum frame changes per second              |
| `keypress_duration`       | 10-5000 ms                    | 100       | How long a paw stays down                     |
| `enable_hand_mapping`     | 0/1                           | 1         | Left keyboard half plays the left paw; 0 picks a random paw |
| `idle_frame`              | 0-4                           | 0         | Frame shown while idle                        |
| `idle_sleep_timeout`      | 0-3600 s                      | 0         | Sleep after this long without input; 0 disables |
| `enable_scheduled_sleep`  | 0/1                           | 0         | Sleep between `sleep_begin` and `sleep_end`   |
| `sleep_begin`, `sleep_end`| HH:MM                         | 00:00     | Daily sleep window; may cross midnight        |
| `test_animation_interval` | 0-3600 s                      | 0         | Tap a paw this often without input; 0 disables |
| `test_animation_duration` | 10-5000 ms                    | 200       | How long a test tap holds the paw             |
| `enable_debug`            | 0/1                           | 0         | Debug logging (never logs keys)               |

### Monitors

Without `monitor=`, one cat follows the focused monitor. Focus comes from the
`wlr-foreign-toplevel-management` protocol, or on Niri from its event stream,
which also reports monitors showing an empty workspace. Without focus
information the cat stays where it is. `follow_active_monitor=0` keeps it on
the first monitor instead. `monitor=eDP-1,HDMI-A-1` or `--monitor eDP-1` shows
a cat on each named monitor, waiting for disconnected ones. Find names with
`bongocat --list-monitors`.

`[monitor:NAME]` sections override the appearance settings, from `cat_height`
to `disable_fullscreen_hide` in the table, for one monitor. `[global]` returns
to global settings:

```ini
monitor=eDP-1,HDMI-A-1
cat_height=40
[monitor:HDMI-A-1]
cat_height=60
mirror_x=1
```

### Fullscreen

A cat hides while the window most recently focused on its monitor is
fullscreen, even when focus moves to another monitor. Switching that monitor
to an empty workspace shows it again; Niri reports where the empty workspace
is, other compositors are assumed to switch the monitor that had focus. The
`overlay` layer and `disable_fullscreen_hide=1` always stay visible.

### Reloading

With `--watch-config`, saving the file reloads it, including atomic
replacements and symlink changes; `bongocat --reload` does the same on demand.
Startup skips invalid lines with a warning. Reloads and `--check-config` are
strict: any invalid line keeps the running configuration.

## Command Line

```
bongocat [OPTIONS]
  -c, --config FILE    Configuration path (searched by default)
  -w, --watch-config   Reload when the configuration changes
  -m, --monitor NAME   Show the cat only on this monitor
  -t, --toggle         Stop the running cat, or start one
  --hide, --show       Hide or show every overlay
  --pause, --resume    Show the idle frame and ignore input, or resume
  --reload, --status   Reload or describe the running cat
  --check-config       Strictly validate the configuration
  --list-devices       List input devices and keyboard settings
  --list-monitors      List monitors, sizes and scales
  --doctor             Check configuration, devices and compositor support
  -h, --help           Show help
  -v, --version        Show version
```

Only one cat runs per user. Control commands talk to it over a socket in
`$XDG_RUNTIME_DIR` that only the same user can use; hidden and paused states
reset when it restarts.

## Troubleshooting

- **Permission denied on input devices:** add yourself to the `input` group
  as shown above, then log in again. `bongocat --list-devices` shows which
  devices are inaccessible.
- **Cat ignores the keyboard:** check `bongocat --status` for the device
  count, and that any `keyboard_device`/`keyboard_name` match your keyboard.
- **Cat on the wrong monitor:** set `monitor=` to a name from
  `bongocat --list-monitors`.
- **Cat over fullscreen windows:** use a layer other than `overlay`.

## Building

```bash
make                  # Release build: build/bongocat
make debug            # AddressSanitizer and UBSan build
make test             # Unit tests
make test-runtime     # Tests against a private compositor; needs
                      # wayland-server, wayland-scanner and python3
sudo make install     # PREFIX=/usr/local by default; DESTDIR supported
```

Requirements: `wayland-client`, `make` and a C23 compiler.
`CC`, `CFLAGS` and `LDFLAGS` are honored. See
[ARCHITECTURE.md](ARCHITECTURE.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT License - see [LICENSE](LICENSE)
