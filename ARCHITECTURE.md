# Architecture

The renderer owns one Wayland connection and one event loop for every overlay.
One input helper is executed with `posix_spawn()` through `/proc/self/exe`.
There are no animation or configuration-watcher threads and no per-monitor
processes. Runtime dependencies remain C23, Linux evdev and Wayland client.

## Ownership and event flow

The renderer polls Wayland, the private input socket, directory inotify,
authenticated control connections and a signal eventfd. Timeouts are the
nearest animation, debounce, helper-recovery, control or Hyprland deadline.
Idle overlays have no animation timeout; sleeping overlays wait for meaningful
deadlines. Animation redraws respect live FPS changes, including `fps=1`.
Wayland read preparation is cancelled whenever a poll wakeup has no display
input. Flush backpressure adds POLLOUT interest.

Each overlay owns a layer surface, optional viewport/fractional-scale object,
two release-aware SHM buffers, effective monitor configuration, animation
state and frame cache. Surface reconstruction and teardown share one path.
Busy retired buffers remain mapped until release. Frame caches rebuild only
when dimensions, scale, mirroring or antialiasing parameters change. Buffer,
stride, scaling, placement and clipping arithmetic uses checked wide values.

`platform/outputs.c` owns stable output slots and protocol metadata.
`platform/wayland.c` reconciles selections, creates and tears down overlays,
and renders them. `platform/shm_buffer.c` owns buffer allocations and release
lifetime. Automatic selection uses one available output; explicit missing
outputs wait while other overlays continue. Configuration reload reconciles
selection without restarting the process. Configure events determine actual
surface dimensions.

`graphics/animation.c` shares parsed embedded SVGs and the rasterizer, while
holding paw deadlines and caches separately for each overlay. Input packets
contain only paw bits, monotonic timestamps and device counts. Configured
mirroring and hand mapping are applied independently for each overlay.
Pause clears activity, displays the idle frame and discards input; resume
resets deadlines. Hide suppresses visibility without stopping animation.

Fullscreen tracking stores every output occupied by a toplevel. State and
output changes are staged until `done`. Output removal and overlay selection
recompute visibility. The optional Hyprland fallback uses asynchronous,
bounded `posix_spawnp()` jobs: one-second deadline, bounded output, checked
exit status and termination/reaping on failure. It does not block Wayland.

## Configuration and control

`config/config.c` parses and validates without input access. Flat files remain
supported. `[monitor:NAME]` overrides appearance; `[global]` returns to global
settings. Overrides are applied after all global settings, independent of
section order. Input and timing stay global. Startup remains tolerant;
strict checking and reload reject malformed, unreadable and missing files.
Reload creates a temporary configuration before swapping the active one.

`config/config_watcher.c` watches the parent directory, filters the basename,
and reloads 300 ms after the last relevant event. Atomic replacement,
deletion/recreation, overflow and invalidated directory watches are handled.

`core/control.c` locks a user-owned regular PID file before truncation, rejects
symlinks and unsafe metadata, and retains its inode between runs. The lock is
held until all cleanup finishes. Controls use a mode-0600 Unix sequenced-packet
socket, SO_PEERCRED same-UID authentication, bounded messages and deadlines.
Toggle requests a stop through the socket; it never trusts a stale PID to
signal an unrelated process group.

## Input and privilege boundaries

The renderer drops real, effective and saved setgid privilege before loading
configuration. Executing the installed binary reacquires its setgid group
only inside helper mode. The helper lowers the effective group while reading
and raises it only for discovery/opening; this also works after helper restart.
No setgid installation or permission grant is performed automatically.

The helper authenticates its inherited socketpair and arms PR_SET_PDEATHSIG
with a parent-race check. `posix_spawn` closes unrelated descriptors. Explicit
paths/names select devices without unrelated fallback. Empty selectors use
EVIOCGBIT keyboard capability queries. Stable aliases are compared by device
identity to avoid duplicates. HUP/ERR/NVAL remove disconnected descriptors;
normal periodic scanning retries connections. Debug never logs keycodes.

SIGTERM, SIGINT, SIGQUIT and SIGHUP wake and stop the renderer. Cleanup stops
input, waits a bounded grace period, escalates to SIGKILL if necessary, reaps
the helper, tears down overlays and finally releases the singleton lock.

## Build and validation

Objects and binaries live under `build/debug` and `build/release`, with
compiler-generated header dependencies. `build/bongocat` selects the build.
`make test` runs deterministic regression suites; `make test-runtime` uses a
small Wayland server fixture (test-only libwayland-server) for multiple outputs,
scale/resolution changes, disconnect/reconnect, release and queue pressure.
`make test-sanitize` checks unit suites with ASan/UBSan; `make debug` also
instruments the real runtime. Release retains PIE, RELRO and stack hardening.
