# wayland-bongocat
#
# CC, CPPFLAGS, CFLAGS and LDFLAGS from the environment are honored and come
# after the project flags, so packagers can override them.

BUILD_TYPE ?= release
PREFIX ?= /usr/local

BUILDDIR := build
OUT := $(BUILDDIR)/$(BUILD_TYPE)
TARGET := $(OUT)/bongocat

WARNINGS := -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes \
	-Wmissing-prototypes -Wold-style-definition -Wredundant-decls \
	-Wnested-externs -Wmissing-include-dirs -Wdouble-promotion -Wshadow
ifeq ($(shell $(CC) -dM -E -x c /dev/null 2>/dev/null | grep -c __clang__),0)
WARNINGS += -Wlogical-op -Wjump-misses-init
endif

PROJECT_CPPFLAGS := -Iinclude -Ilib -Iprotocols
PROJECT_CFLAGS := -std=c2x $(WARNINGS) -fstack-protector-strong

ifeq ($(BUILD_TYPE),debug)
TYPE_CFLAGS := -g3 -O0 -fsanitize=address,undefined
TYPE_LDFLAGS := -fsanitize=address,undefined
TEST_TYPE_CFLAGS := $(TYPE_CFLAGS)
TEST_TYPE_LDFLAGS := $(TYPE_LDFLAGS)
else ifeq ($(BUILD_TYPE),release)
TYPE_CFLAGS := -O3 -flto -fPIE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -DNDEBUG
TYPE_LDFLAGS := -flto -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
TEST_TYPE_CFLAGS := -g -O0
else
$(error BUILD_TYPE must be release or debug)
endif

ALL_CPPFLAGS = $(PROJECT_CPPFLAGS) $(CPPFLAGS)
ALL_CFLAGS = $(PROJECT_CFLAGS) $(TYPE_CFLAGS) $(CFLAGS)
LDLIBS := -lwayland-client -lm

SOURCES := $(wildcard src/*/*.c)
HEADERS := $(wildcard include/*/*.h)
PROTOCOL_SOURCES := $(wildcard protocols/*.c)
ASSETS := $(wildcard assets/svg/*.svg)
OBJECTS := $(SOURCES:src/%.c=$(OUT)/obj/%.o) $(OUT)/obj/graphics/assets.o \
	$(PROTOCOL_SOURCES:protocols/%.c=$(OUT)/obj/protocols/%.o)

.DEFAULT_GOAL := all
.PHONY: all debug release test test-sanitize test-runtime install uninstall \
	clean protocols format format-check lint

# build/bongocat always points at the most recently built variant.
all: $(TARGET)
	@ln -sfn $(BUILD_TYPE)/bongocat $(BUILDDIR)/bongocat

debug release:
	$(MAKE) BUILD_TYPE=$@ all

$(TARGET): $(OBJECTS)
	$(CC) $(ALL_CFLAGS) $(TYPE_LDFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(OUT)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/protocols/%.o: protocols/%.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# The artwork is embedded with .incbin, relative to the repository root.
$(OUT)/obj/graphics/assets.o: src/graphics/assets.S $(ASSETS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

# =============================================================================
# TESTS
# =============================================================================

TESTS := animation config control fullscreen input nanosvg paw_frame scale
TEST_BINARIES := $(TESTS:%=$(OUT)/tests/test_%)
TEST_CFLAGS = $(PROJECT_CFLAGS) $(TEST_TYPE_CFLAGS) $(CFLAGS)

# Extra sources and libraries per test. Tests of static functions include
# the implementation instead.
test_animation_SOURCES := src/graphics/animation.c src/graphics/assets.S \
	src/utils/error.c
test_config_SOURCES := src/config/config.c src/utils/error.c
test_control_SOURCES := src/core/control.c src/config/config_watcher.c
test_fullscreen_SOURCES := \
	protocols/wlr-foreign-toplevel-management-v1-protocol.c src/utils/error.c
test_fullscreen_LIBS := -lwayland-client \
	-Wl,--wrap=wl_proxy_get_version,--wrap=wl_proxy_marshal_flags
test_input_SOURCES := src/utils/error.c
test_input_LIBS := -Wl,--wrap=ioctl,--wrap=stat

.SECONDEXPANSION:
$(OUT)/tests/test_%: tests/test_%.c $$(test_$$*_SOURCES) $(SOURCES) \
		$(HEADERS) $(ASSETS) tests/test_helpers.h
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) -Itests $(TEST_CFLAGS) $< $(test_$*_SOURCES) \
		-o $@ $(TEST_TYPE_LDFLAGS) $(LDFLAGS) $(test_$*_LIBS) -lm

test: $(TEST_BINARIES)
	@failed=0; for test in $^; do \
		echo "--- $${test##*/}"; $$test || failed=$$((failed + 1)); \
	done; \
	[ $$failed -eq 0 ] || { echo "$$failed test(s) failed"; exit 1; }; \
	echo "All tests passed."

test-sanitize:
	$(MAKE) BUILD_TYPE=debug test

# Isolated compositor-fixture tests; need wayland-server and wayland-scanner.
FIXTURE := $(BUILDDIR)/compositor/server
FIXTURE_PROTOCOLS := wlr-layer-shell-unstable-v1 viewporter \
	fractional-scale-v1 wlr-foreign-toplevel-management-unstable-v1

$(BUILDDIR)/compositor/%-server.h: protocols/%.xml
	@mkdir -p $(@D)
	wayland-scanner server-header $< $@

$(FIXTURE): tests/test_compositor.c $(PROTOCOL_SOURCES) \
		$(FIXTURE_PROTOCOLS:%=$(BUILDDIR)/compositor/%-server.h)
	$(CC) -std=c2x -g -Wall -Wextra -I$(BUILDDIR)/compositor $< \
		$(PROTOCOL_SOURCES) -o $@ -lwayland-server

test-runtime: all $(FIXTURE)
	python3 scripts/test_runtime.py
	python3 scripts/test_focus.py

# =============================================================================
# MAINTENANCE
# =============================================================================

install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/bongocat
	install -Dm644 man/bongocat.1 $(DESTDIR)$(PREFIX)/share/man/man1/bongocat.1
	install -Dm644 bongocat.conf.example \
		$(DESTDIR)$(PREFIX)/share/bongocat/bongocat.conf.example

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bongocat
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/bongocat.1
	rm -rf $(DESTDIR)$(PREFIX)/share/bongocat

clean:
	rm -rf $(BUILDDIR)

# Generated bindings are committed; run after changing protocols/*.xml.
protocols:
	for xml in protocols/*.xml; do \
		name=$$(basename $$xml .xml); \
		case $$name in \
		wlr-layer-shell-unstable-v1) name=zwlr-layer-shell-v1 ;; \
		wlr-foreign-toplevel-management-unstable-v1) \
			name=wlr-foreign-toplevel-management-v1 ;; \
		esac; \
		wayland-scanner private-code $$xml protocols/$$name-protocol.c; \
		[ $$name = xdg-shell ] || \
			wayland-scanner client-header $$xml \
				protocols/$$name-client-protocol.h; \
	done

FORMAT_FILES := $(SOURCES) $(HEADERS) $(wildcard tests/*.c tests/*.h)

format:
	clang-format -i $(FORMAT_FILES)

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)

lint:
	clang-tidy --quiet $(SOURCES) -- -std=c2x $(PROJECT_CPPFLAGS)
