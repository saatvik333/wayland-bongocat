# Changelog

All notable changes to this project will be documented in this file.

## [1.4.0] - 2026-02-14

### Added

- **Multi-monitor CSV** - `monitor` now accepts comma-separated output names; parent process launches dedicated children via `--multi-monitor-child` and CLI docs reflect new flow.
- **Safe hot reload hook** - Config watcher now only signals reloads, the actual reload runs from a Wayland tick callback so Wayland structures stay main-thread bound.

### Changed

- **Config model** - `config_t` now stores per-config device/output arrays, inline comments are stripped, and `monitor` parsing loads multiple names with fallback to automatic output; keyboards hotplug state is rebuilt cleanly on reload.
- **Wayland rendering** - Draw path takes `anim_lock`, surface recreation checks the applied layer/output/size snapshots, and buffer recreation is serialized to avoid tearing/races when configs change.
- **Documentation & packaging** - README/`bongocat.conf.example` demonstrate comma-separated monitors, Makefile installs now ship the example config, and CLI help/version text was refreshed for v1.4.0.

### Fixed

- **Dangling pointers on reload** - `g_config` swap, input device lists, and watcher cleanup now occur only after new config is validated so the old config isn’t left referencing freed memory.
- **Config watcher and animation races** - Atomic flags and mutex coverage were tightened, `config_reload_callback` no longer touches Wayland structures directly, and animation draw/update now handle surface recreation safely.
- **Input hotplug robustness** - Path truncation is guarded, static device comparisons check nulls, and signal handlers close cleanly when the child early-exits.

---

## [1.3.2] - 2025-12-07

### Fixed

- **Monitor Reconnection** - Overlay now survives monitor disconnect/reconnect (fixes #15)
- **Dynamic Overlay Resize** - Changing `overlay_height` via config reload no longer crashes
- **Ghost Process Prevention** - Added SIGQUIT/SIGHUP handlers and parent liveness check in child

### Improved

- **Performance Optimizations**
  - Fast buffer clearing using memset (~4x faster)
  - Skip unchanged frames when idle (~95% fewer redraws)
  - Hoisted loop invariants in image scaling
- **Thread Safety** - Mutex protection during buffer recreation
- **Memory Usage** - Reduced from ~20MB to ~8MB RAM

---

## [1.3.1] - 2025-12-06

### Added

- **Keyboard Hand Mapping** - Left half of keyboard triggers left cat hand, right half triggers right hand
- New config option `enable_hand_mapping=1` (enabled by default)
- Hand mapping respects `mirror_x` - hands flip when cat is mirrored

### Changed

- `enable_hand_mapping` default is now `1` (enabled)
- NixOS module `enableHandMapping` default is now `true`

---

## [1.3.0] - 2025-12-06

### Added

- **Improved Anti-Aliasing** - Box filter for downscaling + proper alpha blending for smooth edges at any size
- **Interactive Keyboard Detection** - New `--interactive` mode in `bongocat-find-devices` listens for actual key presses
- **Hot-Reload Device Changes** - Changing keyboard devices in config now works without restart
- **C23 Modern Codebase** - RAII macros, `[[nodiscard]]` attributes, guard clauses throughout

### Fixed

- **Hot-Reload Bug** - Keyboard device path changes now properly trigger input restart
- **Memory Leaks** - Fixed realloc leaks in config parsing
- **Thread Safety** - Atomic operations for shared state between threads
- **Use-After-Free** - Fixed crash in config reload callback
- **Fullscreen Detection** - Now correctly tracks active window per workspace

### Improved

- **README** - Streamlined documentation with minimal config example
- **Script Reliability** - Device detection script distinguishes actual keyboards from power buttons/hotkeys
- **Code Quality** - Organized headers, consistent naming, comprehensive error handling

## [1.2.5] - 2025-08-26

### Added

- **Enhanced Configuration System** - New config variables for fine-tuning appearance and behavior
- **Sleep Mode** - Scheduled or idle-based sleep mode with customizable timing

### Fixed

- **Fixed Positioning** - Fine-tune position, defaults to center

### Improved

- **Default Values** - Refined default configuration values for better out-of-box experience

## [1.2.4] - 2025-08-08

### Added

- **Multi-Monitor Support** - Choose which monitor to display bongocat on using the `monitor` configuration option
- **Monitor Detection** - Automatic detection of available monitors with fallback to first monitor if specified monitor not found
- **XDG Output Protocol** - Proper Wayland protocol implementation for monitor identification

### Fixed

- **Memory Leaks** - Fixed memory leak in monitor configuration cleanup
- **Process Cleanup** - Resolved child process cleanup warnings during shutdown
- **Segmentation Fault** - Fixed crash during application exit related to Wayland resource cleanup

### Improved

- **Error Handling** - Better error messages when specified monitor is not found
- **Resource Management** - Improved cleanup order for Wayland resources
- **Logging** - Enhanced debug logging for monitor detection and selection

## [1.2.3] - 2025-08-02

### Added

- **Smart Fullscreen Detection** - Automatically hides overlay during fullscreen applications for a cleaner experience
- **Enhanced Artwork** - Custom-drawn bongocat image files by [@Shreyabardia](https://github.com/Shreyabardia)
- **Modular Architecture** - Reorganized codebase into logical modules for better maintainability

### Improved

- **Signal Handling** - Fixed duplicate log messages during shutdown
- **Code Organization** - Separated concerns into core, graphics, platform, config, and utils modules
- **Build System** - Updated to support new modular structure

## [1.2.2] - Previous Release

### Added

- Automatic screen detection for all sizes and orientations
- Enhanced performance optimizations

## [1.2.1] - Previous Release

### Added

- Configuration hot-reload system
- Dynamic device detection

## [1.2.0] - Previous Release

### Added

- Hot-reload configuration support
- Dynamic Bluetooth/USB keyboard detection
- Performance optimizations with adaptive monitoring
- Batch processing for improved efficiency

## [1.1.x] - Previous Releases

### Added

- Multi-device support
- Embedded assets
- Cross-platform compatibility (x86_64 and ARM64)
- Basic configuration
