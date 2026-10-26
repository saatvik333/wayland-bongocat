# Repository Guidelines

## Project Structure & Module Organization

`src/` contains C23 implementation code, grouped into `core/`, `config/`,
`graphics/`, `platform/`, and `utils/`; matching public headers live under
`include/`. Unit tests are in `tests/`; compositor-level tests are
`scripts/test_runtime.py` and `scripts/test_focus.py`, which drive the
`tests/test_compositor.c` fixture. Wayland XML definitions and committed
generated bindings live in `protocols/`, vendored NanoSVG headers in `lib/`,
and the cat artwork in `assets/svg/`, embedded by `src/graphics/assets.S`.
Packaging is under `nix/`. See `ARCHITECTURE.md` before changing process,
event-loop, or Wayland lifecycle code.

## Build, Test, and Development Commands

- `make` / `make release` builds the optimized, hardened `build/release/bongocat`.
- `make debug` builds `build/debug/bongocat` with ASan and UBSan.
- `make test` runs the unit tests; `make test-sanitize` runs them under
  ASan/UBSan.
- `make test-runtime` runs the compositor-fixture tests (needs
  wayland-server, wayland-scanner, python3).
- `make format` / `make format-check` apply or verify `.clang-format`.
- `make lint` runs `clang-tidy`.
- `./build/bongocat -c bongocat.conf.example -w` runs a local build with
  config watching.

Run `make protocols` only after changing protocol XML; do not hand-edit
generated protocol files. Edit artwork in `assets/svg/` directly.

## Coding Style & Naming Conventions

Use two-space indentation, attached braces, and an 80-column limit for C.
Follow `.editorconfig` and `.clang-format`; Makefile recipes require tabs.
Functions, variables, structs, and enums use `lower_case`; macros, global
constants, and enum constants use `UPPER_CASE`; typedefs end in `_t`. Keep
headers paired with their module and reuse existing helpers (`utils/clock.h`,
`utils/error.h`).

## Testing Guidelines

Tests are standalone C programs using `TEST_ASSERT` from
`tests/test_helpers.h`. Name new files `tests/test_<module>.c`, add the name
to `TESTS` and any extra sources to `test_<name>_SOURCES` in the Makefile.
Before submitting, run `make format-check`, `make release`,
`make clean && make debug`, `make test-sanitize`, `make test-runtime` and
`make lint`, matching CI.

## Commit & Pull Request Guidelines

Use Conventional Commits seen in history: `feat:`, `fix:`, `docs:`,
`refactor:`, or scoped forms such as `fix(build):`. Keep each commit focused.
Pull requests should explain behavior and motivation, link relevant issues,
list validation commands, and note compositor/config details for runtime bugs.
Include a screenshot or recording for visible overlay changes.

## Security & Configuration Tips

Never commit personal device paths or live config. Keycodes must never leave
the input helper or appear in logs. Preserve `/dev/input/` path validation, the
setgid privilege drop, and the existing overflow, lock-file, and
process-execution safeguards.
