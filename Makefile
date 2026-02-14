# Compiler
CC = gcc

# Build type (debug or release)
BUILD_TYPE ?= release

# Base flags (using c2x for C23 compatibility on GCC 9+)
BASE_CFLAGS = -std=c2x -Iinclude -Ilib -Iprotocols
BASE_CFLAGS += -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes
BASE_CFLAGS += -Wmissing-prototypes -Wold-style-definition -Wredundant-decls
BASE_CFLAGS += -Wnested-externs -Wmissing-include-dirs -Wlogical-op
BASE_CFLAGS += -Wjump-misses-init -Wdouble-promotion -Wshadow
BASE_CFLAGS += -fstack-protector-strong -D_FORTIFY_SOURCE=2

# Debug flags
DEBUG_CFLAGS = $(BASE_CFLAGS) -g3 -O0 -DDEBUG -fsanitize=address -fsanitize=undefined
DEBUG_LDFLAGS = -fsanitize=address -fsanitize=undefined

# Release flags  
RELEASE_CFLAGS = $(BASE_CFLAGS) -O3 -DNDEBUG -flto -march=native
RELEASE_CFLAGS += -fomit-frame-pointer -funroll-loops -finline-functions

# Set flags based on build type
ifeq ($(BUILD_TYPE),debug)
    CFLAGS = $(DEBUG_CFLAGS)
    LDFLAGS = -lwayland-client -lm -lpthread $(DEBUG_LDFLAGS)
else
    CFLAGS = $(RELEASE_CFLAGS)
    LDFLAGS = -lwayland-client -lm -lpthread -flto
endif

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build
OBJDIR = $(BUILDDIR)/obj
PROTOCOLDIR = protocols
WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols

# Source files (including embedded assets which are now committed)
SOURCES = $(shell find $(SRCDIR) -name "*.c")
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Embedded assets (now committed to git, use embed_assets.sh manually when assets change)
EMBED_SCRIPT = scripts/embed_assets.sh
EMBEDDED_ASSETS_H = $(INCDIR)/graphics/embedded_assets.h
EMBEDDED_ASSETS_C = $(SRCDIR)/graphics/embedded_assets.c

# Protocol files
C_PROTOCOL_SRC = $(PROTOCOLDIR)/zwlr-layer-shell-v1-protocol.c $(PROTOCOLDIR)/xdg-shell-protocol.c $(PROTOCOLDIR)/wlr-foreign-toplevel-management-v1-protocol.c $(PROTOCOLDIR)/xdg-output-unstable-v1-protocol.c
H_PROTOCOL_HDR = $(PROTOCOLDIR)/zwlr-layer-shell-v1-client-protocol.h $(PROTOCOLDIR)/xdg-shell-client-protocol.h $(PROTOCOLDIR)/wlr-foreign-toplevel-management-v1-client-protocol.h $(PROTOCOLDIR)/xdg-output-unstable-v1-client-protocol.h
PROTOCOL_OBJECTS = $(C_PROTOCOL_SRC:$(PROTOCOLDIR)/%.c=$(OBJDIR)/%.o)

# Target executable
TARGET = $(BUILDDIR)/bongocat

.PHONY: all clean protocols embed-assets format format-check lint

all: protocols $(TARGET)

# Generate protocol files first
protocols: $(C_PROTOCOL_SRC) $(H_PROTOCOL_HDR)

# Generate embedded assets (manual target - run when assets change)
embed-assets: 
	./$(EMBED_SCRIPT)

# Create build directories
$(OBJDIR):
	mkdir -p $(OBJDIR)
	mkdir -p $(OBJDIR)/core
	mkdir -p $(OBJDIR)/graphics
	mkdir -p $(OBJDIR)/platform
	mkdir -p $(OBJDIR)/config
	mkdir -p $(OBJDIR)/utils
	mkdir -p $(BUILDDIR)

# Compile source files (depends on protocol headers)
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(H_PROTOCOL_HDR) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile protocol files
$(OBJDIR)/%.o: $(PROTOCOLDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS) $(PROTOCOL_OBJECTS)
	$(CC) $(OBJECTS) $(PROTOCOL_OBJECTS) -o $(TARGET) $(LDFLAGS)

# Rule to generate Wayland protocol files
$(C_PROTOCOL_SRC) $(H_PROTOCOL_HDR): $(PROTOCOLDIR)/wlr-layer-shell-unstable-v1.xml $(PROTOCOLDIR)/wlr-foreign-toplevel-management-unstable-v1.xml $(PROTOCOLDIR)/xdg-output-unstable-v1.xml
	wayland-scanner client-header $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $(PROTOCOLDIR)/xdg-shell-client-protocol.h
	wayland-scanner private-code $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $(PROTOCOLDIR)/xdg-shell-protocol.c
	wayland-scanner private-code $(PROTOCOLDIR)/wlr-layer-shell-unstable-v1.xml $(PROTOCOLDIR)/zwlr-layer-shell-v1-protocol.c
	wayland-scanner client-header $(PROTOCOLDIR)/wlr-layer-shell-unstable-v1.xml $(PROTOCOLDIR)/zwlr-layer-shell-v1-client-protocol.h
	wayland-scanner private-code $(PROTOCOLDIR)/wlr-foreign-toplevel-management-unstable-v1.xml $(PROTOCOLDIR)/wlr-foreign-toplevel-management-v1-protocol.c
	wayland-scanner client-header $(PROTOCOLDIR)/wlr-foreign-toplevel-management-unstable-v1.xml $(PROTOCOLDIR)/wlr-foreign-toplevel-management-v1-client-protocol.h
	wayland-scanner client-header $(PROTOCOLDIR)/xdg-output-unstable-v1.xml $(PROTOCOLDIR)/xdg-output-unstable-v1-client-protocol.h
	wayland-scanner private-code $(PROTOCOLDIR)/xdg-output-unstable-v1.xml $(PROTOCOLDIR)/xdg-output-unstable-v1-protocol.c

clean:
	rm -rf $(BUILDDIR) $(C_PROTOCOL_SRC) $(H_PROTOCOL_HDR)

# Development targets
debug:
	$(MAKE) BUILD_TYPE=debug

release:
	$(MAKE) BUILD_TYPE=release

install: $(TARGET)
	install -D $(TARGET) $(DESTDIR)/usr/local/bin/bongocat
	install -D bongocat.conf.example $(DESTDIR)/usr/local/share/bongocat/bongocat.conf.example
	install -D scripts/find_input_devices.sh $(DESTDIR)/usr/local/bin/bongocat-find-devices
	install -D man/bongocat.1 $(DESTDIR)/usr/local/share/man/man1/bongocat.1

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/bongocat
	rm -f $(DESTDIR)/usr/local/bin/bongocat-find-devices
	rm -rf $(DESTDIR)/usr/local/share/bongocat

# Memory check (requires valgrind)
memcheck: debug
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET)

# Performance profiling
profile: release
	perf record -g ./$(TARGET)
	perf report

.PHONY: debug release install uninstall analyze memcheck profile format format-check lint

# =============================================================================
# CODE QUALITY TARGETS
# =============================================================================

# Find all project source files (exclude lib/ and protocols/)
PROJECT_SOURCES = $(shell find $(SRCDIR) -name '*.c' ! -path '*/embedded_assets.c')
PROJECT_HEADERS = $(shell find $(INCDIR) -name '*.h')
ALL_PROJECT_FILES = $(PROJECT_SOURCES) $(PROJECT_HEADERS)

# Format all project source files
format:
	@echo "Formatting source files..."
	@clang-format -i $(ALL_PROJECT_FILES)
	@echo "Done! Formatted $(words $(ALL_PROJECT_FILES)) files."

# Check if formatting is correct (for CI)
format-check:
	@echo "Checking code formatting..."
	@clang-format --dry-run --Werror $(ALL_PROJECT_FILES)
	@echo "All files are properly formatted."

# Static analysis with clang-tidy (uses .clang-tidy config)
lint: protocols
	@echo "Running static analysis..."
	@clang-tidy $(PROJECT_SOURCES) -- $(CFLAGS) 2>/dev/null || true
	@echo "Static analysis complete."

# Alias for lint
analyze: lint

# Generate compile_commands.json for IDE support (requires bear)
# Run: make compiledb
compiledb: clean
	@echo "Generating compile_commands.json..."
	@bear -- $(MAKE) all 2>/dev/null || (echo "Note: 'bear' not installed. Install with: sudo pacman -S bear" && false)
	@echo "compile_commands.json generated!"

.PHONY: compiledb
