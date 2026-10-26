# Architecture

Two processes, each single-threaded:

- The **renderer** owns the Wayland connection and draws one overlay per
  selected monitor.
- The **input helper** (`bongocat --input-helper`, spawned through
  `/proc/self/exe`) reads keyboards and sends the renderer only paw bits,
  timestamps and device counts.

Runtime dependencies are `libwayland-client`, libc and Linux evdev.

## Event loop

`core/main.c` owns the only `poll()`. Each turn:

1. `wayland_prepare()` dispatches queued Wayland events, reconciles overlays,
   advances animations, draws what changed, flushes, and returns the display
   descriptor plus the next animation deadline.
2. `poll()` waits on the display, a signal eventfd, the helper socket, the
   config inotify descriptor, the niri stream, the sleep-schedule timerfd and
   the control sockets, until the earliest deadline (animation, reload
   debounce, control client timeout, helper restart).
3. `wayland_dispatch()` reads display events; every other source is handled
   only when its descriptor fired or its deadline passed.

An idle cat has no deadline at all, so it sleeps in `poll()` until something
happens. Scheduled sleep uses a `CLOCK_REALTIME` timerfd armed for the next
boundary, which also fires correctly after suspend or clock changes. When the
Wayland socket is full, the renderer stops queueing requests and waits for
`POLLOUT` while controls keep working.

## Overlays (`platform/wayland.c`)

Each overlay owns a layer surface, optional viewport and fractional-scale
objects, two release-aware SHM buffers (`platform/shm_buffer.c`), its
appearance, animation state and frame cache. Outputs live in fixed slots
(`platform/outputs.c`); overlays use the same slot index.

Reconciliation runs after output, configuration or focus changes:

- Explicit `monitor=` names create one overlay per available name.
- Otherwise one overlay follows the focused output (or the first output with
  `follow_active_monitor=0`). Moving it hands the animation state to the new
  surface so timers continue.
- Appearance changes update the surface in place; a layer change recreates it.
- An overlay the compositor closes, or that cannot allocate buffers, is
  dropped until outputs or the configuration change, so a misbehaving output
  cannot cause a recreate loop or stop the others.

Drawing is incremental. Each buffer remembers which appearance generation and
visibility it holds; when current, only the cat's rectangle is repainted and
damaged. Frame changes are limited to `fps`; visibility and appearance changes
draw immediately. A hidden overlay commits one transparent buffer and stops
drawing.

## Animation (`graphics/animation.c`)

The five SVG frames are embedded with `.incbin` (`src/graphics/assets.S`) and
parsed once. Each overlay rasterizes them at its own size and orientation into
a `frame_cache_t`. `anim_step()` is a pure function of the overlay's
`anim_state_t`, the configuration, the paws pressed since the last turn, and
the time; it returns the next time the frame can change. Paws are mirrored per
overlay; with `enable_hand_mapping=0` one random paw is chosen per input batch
so every monitor agrees.

## Focus and fullscreen

`platform/fullscreen.c` tracks toplevels from wlr-foreign-toplevel-management.
State, output enter/leave and activation are staged until `done`; title and
app-id updates are ignored. Each activation gets an increasing order number.

- **Fullscreen hiding:** an output is covered when the window most recently
  activated on it is fullscreen, so a fullscreen video stays covered while
  focus is on another monitor.
- **Empty workspaces:** focusing an empty workspace uncovers its output. niri
  reports which output that is (`platform/niri.c`). Elsewhere, when a batch of
  events leaves no window activated, the outputs of the windows that lost
  focus are forgotten (every output, if the compositor sends no output
  events): switching workspaces usually happens on the focused monitor.
- **Following:** automatic selection uses niri's focused output when known,
  else the output of the most recently activated window (preferring the
  current one for spanning windows), else stays put.

`platform/niri.c` subscribes to `NIRI_SOCKET`'s event stream while following
or fullscreen hiding needs it. It tracks workspaces, the focused one and
whether it holds a window. Lines longer than its buffer are discarded;
malformed lines or a closed stream clear what niri reported.

## Configuration (`config/`)

`config.c` is table-driven: each key has one entry with its type, field
offset, range or choices. Appearance keys are those whose field lives in
`appearance_t`; `[monitor:NAME]` sections store them as typed overrides with a
bitmask of the fields set, so `config_appearance()` only copies values. Keys
under an invalid section header are ignored. Startup skips invalid lines with
warnings (clamping out-of-range numbers); strict loading (reload,
`--check-config`) rejects the whole file. Obsolete keys are accepted with a
warning.

`config_watcher.c` watches the file (following symlinks) and its directory, so
in-place writes, atomic replacement, deletion, recreation and symlink
retargeting all trigger a reload 300 ms after the last event. Reload builds a
new `config_t` and swaps it in only on success.

## Control (`core/control.c`)

A user-owned regular lock file in `$XDG_RUNTIME_DIR` (symlinks and unsafe
metadata rejected) keeps one instance per user; its inode survives restarts.
Controls use a mode-0600 `SOCK_SEQPACKET` socket, same-UID `SO_PEERCRED`
checks, bounded requests and a one-second client deadline. `--toggle` stops
the running instance through the socket, never by PID.

## Input and privileges (`platform/input.c`)

For setgid installations the renderer drops real, effective and saved group
privilege before loading configuration; `--list-devices` opens devices before
that. The helper regains the group only while opening devices.

The helper authenticates its inherited socket, closes inherited descriptors,
and exits when the renderer's socket hangs up. Without selectors it opens
every device with keyboard keys (`EVIOCGBIT`); explicit paths match by device
number, so `by-id` and `by-path` links work and duplicates are skipped. An
inotify watch on `/dev/input` rescans 100 ms after devices appear or change
permissions, so hotplug needs no timer. Devices that hang up or fail are
closed. Only key-press events are turned into paw bits; keycodes never leave
the helper.

When the helper exits, the renderer sees end-of-file, reaps it and restarts it
after at most five seconds.

## Build and tests

`make` builds `build/release/bongocat`; `make debug` builds
`build/debug/bongocat` with ASan and UBSan. Test binaries live in the
variant's directory. `make test` runs the unit tests; `make test-runtime` runs
`scripts/test_runtime.py` and `scripts/test_focus.py` against
`tests/test_compositor.c`, a small Wayland server covering multiple outputs,
scale and resolution changes, hotplug, focus, fullscreen, buffer release and
queue pressure. Its phases advance on `SIGUSR1`, so tests never race timers.
