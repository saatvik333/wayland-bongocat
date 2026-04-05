# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Bongo Cat Wayland Overlay — a lightweight C23 application (~8MB RAM, ~5,300 lines) that renders an animated cat overlay on Wayland compositors, reacting to keyboard input. Uses wlr-layer-shell for overlay positioning and reads `/dev/input/eventX` devices directly.

For detailed architecture, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Build Commands

```bash
make                # Release build with PIE + RELRO hardening (output: build/bongocat)
make debug          # Debug build with ASan + UBSan
make release-local  # Release with -march=native (local only, not for distribution)
make tsan           # ThreadSanitizer build for data race detection
make test           # Run unit tests (config parser + memory pool)
make clean          # Remove build artifacts (keeps generated protocol files)
make distclean      # Remove build artifacts + generated protocol files
make format         # clang-format all src/ and include/ files (excludes embedded_assets.c)
make format-check   # Verify formatting without modifying (CI)
make lint           # clang-tidy static analysis (alias: make analyze)
make compiledb      # Generate compile_commands.json (requires bear)
make memcheck       # Valgrind leak check (builds debug first)
make profile        # perf record + report (builds release first)
make protocols      # Regenerate Wayland protocol bindings (requires wayland-scanner)
make embed-assets   # Re-embed SVG assets (run only when SVGs in assets/new/ change)
make install        # Install to /usr/local (respects DESTDIR)
```

Run the binary: `./build/bongocat -c bongocat.conf.example -w`

## Module Layout

| Directory | Purpose |
|-----------|---------|
| `src/core/` | Entry point (`main.c`), multi-monitor forking (`multi_monitor.c`) |
| `src/config/` | INI-style config parsing (`config.c`), inotify watcher (`config_watcher.c`) |
| `src/platform/` | Wayland core (`wayland.c`), fullscreen detection (`fullscreen.c`), Hyprland IPC (`hyprland.c`), input (`input.c`) |
| `src/graphics/` | SVG rasterization, frame caching, state machine (`animation.c`), embedded SVG data (`embedded_assets.c`) |
| `src/utils/` | Error codes/logging (`error.c`), tracked allocation and memory pools (`memory.c`) |
| `protocols/` | Wayland XML specs + committed C bindings (regenerate with `make protocols`) |
| `tests/` | Unit tests for config parser and memory pool |
| `lib/` | Vendored `nanosvg.h` + `nanosvgrast.h` for SVG parsing and rasterization |

## Key Architecture Points

- **Multi-process**: parent forks one child per monitor via `execvp`; each has its own Wayland connection
- **IPC**: input child communicates key state via `MAP_SHARED` anonymous mmap atomics + `eventfd` wake
- **SVG rendering**: 5 frames (including sleep) rasterized from SVG by nanosvg at init/config-reload; pixel-perfect at any size
- **Frame caching**: cached frames stored as BGRA; `draw_bar()` does BGRA-to-BGRA blit (no per-frame scaling)
- **Idle power**: animation thread uses `poll()` on eventfd with 1s timeout (1 wake/sec idle vs 30 previously)
- **Locking**: single `anim_lock` mutex; `wl_display_flush()` called outside the lock

## Code Conventions

- **C23** (`-std=c2x`), compiled with GCC; use `[[maybe_unused]]` not `__attribute__((unused))`
- **Naming**: `lower_case` functions/variables, `UPPER_CASE` macros/constants, `_t` suffix on typedefs
- **Atomics**: use `atomic_bool`/`atomic_int` (stdatomic.h typedefs), not `_Atomic` keyword form
- **Commit messages**: conventional commits (`feat:`, `fix:`, `docs:`, `refactor:`)
- **Format before committing**: `make format`
- **Guard clauses**: use macros from `error.h` for null/error checking (`BONGOCAT_CHECK_NULL`, `BONGOCAT_CHECK_ERROR`)
- **Memory**: `BONGOCAT_AUTO_FREE` attribute for scope-based cleanup in debug builds; `memory_pool_t` for batch allocation
- **No `popen`**: use `fork()`/`execvp()` for subprocess execution (security requirement)

## Key Details

- `embedded_assets.c` is auto-generated (SVG byte arrays) — don't edit by hand; regenerate with `make embed-assets`
- Config file searched at `$XDG_CONFIG_HOME/bongocat/bongocat.conf`, then `~/.config/bongocat/bongocat.conf`, then `./bongocat.conf`
- PID file at `$XDG_RUNTIME_DIR/bongocat.pid` with `O_NOFOLLOW` and mode 0600; `--toggle` sends SIGTERM
- `keyboard_device` paths validated to require `/dev/input/` prefix (path traversal rejected)
- Release builds include PIE, full RELRO, noexecstack, stack protector, and `_FORTIFY_SOURCE=2`
- CI runs on push/PR: format-check, release build, debug build, tests
- AUR action pinned to SHA with `permissions: { contents: read }`
- Nix support via `flake.nix` with NixOS module and Home Manager module in `nix/`
