# Repository Guidelines

## Project Structure & Module Organization

`src/` contains C23 implementation code, grouped into `core/`, `config/`,
`graphics/`, `platform/`, and `utils/`; matching public headers live under
`include/`. Unit tests are in `tests/`. Wayland XML definitions and committed
generated bindings live in `protocols/`, vendored NanoSVG headers in `lib/`,
and cat artwork in `assets/`. Packaging is under `nix/`; helper utilities are
under `scripts/`. See `ARCHITECTURE.md` before changing process, threading, or
Wayland lifecycle code.

## Build, Test, and Development Commands

- `make debug` builds `build/bongocat` with debug symbols, ASan, and UBSan.
- `make release` creates the optimized, hardened release binary.
- `make test` builds and runs all unit-test executables.
- `make format-check` verifies formatting without changing files.
- `make format` applies `.clang-format` to project C sources and headers.
- `make lint` runs `clang-tidy`; `make memcheck` runs the debug binary through
  Valgrind.
- `./build/bongocat -c bongocat.conf.example -w` runs a local build with config
  watching.

Run `make protocols` only after changing protocol XML. Run `make embed-assets`
after changing embedded SVG assets; do not hand-edit generated protocol files
or `src/graphics/embedded_assets.c`.

## Coding Style & Naming Conventions

Use two-space indentation, attached braces, and an 80-column limit for C.
Follow `.editorconfig` and `.clang-format`; Makefile recipes require tabs.
Functions, variables, structs, and enums use `lower_case`; macros, global
constants, and enum constants use `UPPER_CASE`; typedefs end in `_t`. Keep
headers paired with their module and reuse existing error and allocation
helpers.

## Testing Guidelines

Tests are standalone C programs using the repository's `TEST_ASSERT` macros.
Name new files `tests/test_<module>.c`, add their binary and dependencies to the
Makefile, and return nonzero on failure. Add focused tests for parsing,
validation, memory, and other deterministic logic. No coverage threshold is
defined. Before submitting, run `make format-check`, `make release`,
`make clean && make debug`, and `make test`, matching CI.

## Commit & Pull Request Guidelines

Use Conventional Commits seen in history: `feat:`, `fix:`, `docs:`,
`refactor:`, or scoped forms such as `fix(build):`. Keep each commit focused.
Pull requests should explain behavior and motivation, link relevant issues,
list validation commands, and note compositor/config details for runtime bugs.
Include a screenshot or recording for visible overlay changes.

## Security & Configuration Tips

Never commit personal device paths or live config. Debug logging exposes
keycodes; keep `enable_debug=0` outside diagnosis. Preserve `/dev/input/` path
validation and existing overflow, PID-file, and process-execution safeguards.
