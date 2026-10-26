# Contributing

## Setup

You need `wayland-client`, `make` and a C23 compiler. The runtime tests also
need `wayland-server`, `wayland-scanner` and `python3`. Generated protocol
bindings are committed; `make protocols` regenerates them after changing
`protocols/*.xml`.

```bash
git clone https://github.com/saatvik333/wayland-bongocat.git
cd wayland-bongocat
make debug
./build/bongocat -c bongocat.conf.example -w
```

Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing the event loop,
Wayland lifecycle or input helper.

## Before a pull request

```bash
make format-check
make release
make clean && make debug
make test-sanitize
make test-runtime
make lint
```

- Use Conventional Commits (`feat:`, `fix:`, `docs:`, `refactor:`,
  `fix(input):` …), one topic per commit.
- Describe the behavior change and its motivation, link issues, and list the
  commands you ran.
- For visible changes, include a screenshot or recording.
- For runtime bugs, include your compositor and its version, your
  configuration, and output with `enable_debug=1`.

## Tests

Unit tests are standalone programs in `tests/test_<module>.c` using
`TEST_ASSERT` from `tests/test_helpers.h`. Add the test name to `TESTS` in the
Makefile, and its extra sources to `test_<name>_SOURCES`. Behavior involving
the compositor belongs in `scripts/test_runtime.py` or `scripts/test_focus.py`.
