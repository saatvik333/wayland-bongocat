# Architecture

## Overview

Bongo Cat Wayland Overlay is a lightweight C23 application (~5,300 lines of hand-written code) that renders an animated cat overlay on Wayland compositors, reacting to keyboard input in real-time.

```
                    +-----------------------+
                    |    Parent Process     |
                    |  (multi_monitor.c)    |
                    +----------+------------+
                               |
               fork + execvp per monitor
                               |
          +--------------------+---------------------+
          |                                          |
  +-------v---------+                       +--------v--------+
  | Monitor Child 1 |                       | Monitor Child N |
  |                 |                       |                 |
  |  +-----------+  |                       |  +-----------+  |
  |  | Wayland   |  |  Main thread          |  | Wayland   |  |
  |  | Event     |<-+- (poll loop)          |  | Event     |  |
  |  | Loop      |  |                       |  | Loop      |  |
  |  +-----------+  |                       |  +-----------+  |
  |                 |                       |                 |
  |  +-----------+  |                       |  +-----------+  |
  |  | Animation |  |  pthread              |  | Animation |  |
  |  | Thread    |<-+- (eventfd wake)       |  | Thread    |  |
  |  +-----------+  |                       |  +-----------+  |
  |                 |                       |                 |
  |  +-----------+  |                       |  +-----------+  |
  |  | Config    |  |  pthread              |  | Config    |  |
  |  | Watcher   |<-+- (inotify)            |  | Watcher   |  |
  |  +-----------+  |                       |  +-----------+  |
  |                 |                       |                 |
  |  +-----------+  |                       |  +-----------+  |
  |  | Input     |  |  fork                 |  | Input     |  |
  |  | Child     |<-+- (shared mmap)        |  | Child     |  |
  |  +-----------+  |                       |  +-----------+  |
  +-----------------+                       +-----------------+
```

## Process Model

### Multi-Monitor Mode

The parent process (`multi_monitor.c`) forks one child per configured monitor via `fork()` + `execvp()` with `--monitor NAME`. Each child is a fully independent instance with its own Wayland connection, animation thread, and input monitor. Re-executing (not just forking) avoids inheriting the parent's Wayland state.

### Single-Monitor Mode

A single process handles everything. No `execvp()`.

### Per-Instance Architecture

Each instance runs 3 threads + 1 child process:

| Component | Type | Purpose |
|-----------|------|---------|

| **Main thread** | Wayland event loop | `poll()` on `wl_display` fd, dispatches protocol events, handles config reload ticks |
| **Animation thread** | pthread | Runs frame state machine, calls `draw_bar()` when frame changes, sleeps via `eventfd` when idle |
| **Config watcher** | pthread | `inotify` on config file, debounces (300ms), triggers hot-reload |
| **Input child** | fork | Reads `/dev/input/eventX` via `poll()`, writes atomic key state + eventfd wake signal |

## Data Flow

```
/dev/input/eventX
       |
       v
  Input Child Process
  (poll on evdev fds)
       |
       | atomic_store(any_key_pressed, 1)
       | atomic_store(last_key_code, code)
       | write(eventfd)  -- wake animation thread
       |
       v
  Animation Thread
  (poll on eventfd, nanosleep at FPS rate)
       |
       | anim_update_state() under anim_lock
       | selects frame 0-4 based on key + hand mapping + sleep state
       |
       v
  draw_bar() under anim_lock
       |
       | memset pixels buffer (alpha fill)
       | blit_cached_frame() -- pre-scaled BGRA copy
       | wl_surface_commit()
       |
       v
  wl_display_flush()  -- outside anim_lock
       |
       v
  Wayland Compositor renders overlay
```

## Module Layout

```
src/
  core/
    main.c              (810 lines)  Entry point, PID file, signal handling, cleanup
    multi_monitor.c     (164 lines)  Fork/exec per monitor, child management
  config/
    config.c            (837 lines)  INI parser, validation, defaults, XDG path resolution
    config_watcher.c    (237 lines)  inotify thread with debounce and re-watch
  platform/
    wayland.c          (1129 lines)  Core Wayland: registry, surface, buffer, draw_bar
    fullscreen.c        (415 lines)  Foreign-toplevel fullscreen detection
    hyprland.c          (135 lines)  Hyprland IPC fallback (fork/execvp, not popen)
    input.c             (493 lines)  evdev reading, shared memory IPC, eventfd
  graphics/
    animation.c         (580 lines)  Frame state machine, SVG rasterization, caching, thread
    embedded_assets.c                Auto-generated SVG byte arrays (do not edit)
  utils/
    error.c              (94 lines)  Logging with timestamps, atomic debug flag
    memory.c            (242 lines)  Tracked allocator, memory pools, leak checker

include/                (756 lines)  Public headers for each module
tests/                  (450 lines)  Unit tests for config parser and memory pool
protocols/                           Generated Wayland protocol C bindings
lib/                                 Vendored nanosvg.h + nanosvgrast.h for SVG rendering
```

## Wayland Protocol Stack

Four protocols generated from XML via `wayland-scanner`:

| Protocol | Purpose |
|----------|---------|

| **wlr-layer-shell** | Positions the overlay on a specific layer (top/overlay) with anchoring |
| **xdg-output** | Enumerates monitors by name for multi-monitor targeting |
| **wlr-foreign-toplevel-management** | Detects fullscreen windows to auto-hide the overlay |
| **xdg-shell** | Standard shell surface (base requirement) |

Version negotiation uses `MIN(advertised, desired)` to handle compositors with older protocol versions.

## Synchronization

| Mechanism | Protects | Scope |
|-----------|----------|-------|

| `anim_lock` (pthread_mutex) | `anim_index`, `pixels`, `surface`, `buffer`, `current_config` pointer, cached frames | Animation thread + Wayland main thread |
| `atomic_int any_key_pressed` | Key press flag | Input child -> Animation thread (via `MAP_SHARED` mmap) |
| `atomic_int last_key_code` | Last keycode for hand mapping | Input child -> Animation thread (via `MAP_SHARED` mmap) |
| `atomic_bool configured` | Surface ready flag | Wayland callbacks -> Animation thread |
| `atomic_bool fullscreen_detected` | Fullscreen state | Fullscreen module -> draw_bar() |
| `atomic_bool g_reload_pending` | Config change flag | Config watcher -> Main thread tick |
| `eventfd` (EFD_NONBLOCK) | Animation wake-up | Input child writes -> Animation thread polls |

### Lock ordering

`anim_lock` is the only mutex. It is acquired in:

- `draw_bar()` (animation thread + Wayland callbacks)
- `anim_update_state()` (animation thread)
- `wayland_update_config()` (main thread during config reload)

`wl_display_flush()` is called outside the lock to avoid blocking the mutex on a write() syscall.

## Performance Characteristics

| Metric | Value |
|--------|-------|
| **Memory** | ~8MB RSS |
| **Idle CPU** | ~0% (1 wake/sec via eventfd timeout) |
| **Active CPU** | Minimal (pre-scaled frame cache, ~15KB memcpy per frame) |
| **Startup** | ~20ms (SVG parse + rasterization of 5 embedded SVGs at target size) |
| **Frame latency** | <1ms (cached blit + Wayland commit) |
| **Binary size** | ~300KB (with embedded SVG assets + nanosvg rasterizer) |
| **Per-monitor overhead** | Separate process (~8MB each) |

### Frame Caching

SVGs (500x277 viewBox) are rasterized by nanosvg directly at target display dimensions at startup and on config reload. The 5 cached frames (including sleep) are stored in BGRA format (Wayland-native). `draw_bar()` performs a direct BGRA-to-BGRA blit without channel conversion or scaling math. Since SVGs are vector graphics, rendering is pixel-perfect at any size with built-in anti-aliasing.

### Idle Power

The animation thread uses `poll()` on an `eventfd` with a 1-second timeout when idle. The input child writes to the eventfd on keypress for immediate wake-up. This replaces the previous 30Hz polling loop.

## Security Model

- The application requires `input` group membership to read `/dev/input/eventX` devices directly (no Wayland protocol exists for passive keyboard monitoring)
- `keyboard_device` config paths are validated to require `/dev/input/` prefix with path traversal rejection
- PID file stored in `$XDG_RUNTIME_DIR` with `O_NOFOLLOW` and mode 0600
- Hyprland IPC uses `fork()/execvp()` instead of `popen()` to avoid shell injection
- Integer config values validated with `strtol()` + endptr/errno checking
- Buffer size calculations use `size_t` with overflow protection
- Release builds include PIE, full RELRO, and non-executable stack
- `enable_debug=1` logs keycodes to stdout -- this is a keylogger; documented in config with WARNING

## Build Hardening

Release builds (`make release`) include:

- `-O3 -flto` -- Link-time optimization
- `-fPIE` / `-pie` -- Position-independent executable (ASLR)
- `-Wl,-z,relro,-z,now` -- Full RELRO (GOT protection)
- `-Wl,-z,noexecstack` -- Non-executable stack
- `-fstack-protector-strong` -- Stack canaries
- `-D_FORTIFY_SOURCE=2` -- Buffer overflow detection

Debug builds (`make debug`) include ASan + UBSan. TSan available via `make tsan`.
