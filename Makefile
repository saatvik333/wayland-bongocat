# Compiler
CC = gcc

# Try to find XML files via pkg-config, otherwise fall back
PKG_PREFIX := $(shell pkg-config --variable=prefix wayland-protocols 2>/dev/null)
# construct the XML path, or else default to /usr/share/wayland-protocols
PROTO_DIR  := $(if $(PKG_PREFIX),$(PKG_PREFIX)/share/wayland-protocols,/usr/share/wayland-protocols)



# Build type (debug or release)
BUILD_TYPE ?= release

# Base flags
BASE_CFLAGS = -std=c11 -Iinclude -Ilib -Iprotocols
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
OBJDIR = obj
PROTOCOLDIR = protocols

# Source files (excluding embedded assets which is generated)
SOURCES = $(filter-out $(EMBEDDED_ASSETS_C), $(wildcard $(SRCDIR)/*.c))
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o) $(OBJDIR)/embedded_assets.o

# Embedded assets
EMBED_SCRIPT = scripts/embed_assets.sh
EMBEDDED_ASSETS_H = $(INCDIR)/embedded_assets.h
EMBEDDED_ASSETS_C = $(SRCDIR)/embedded_assets.c

# Protocol files
C_PROTOCOL_SRC = $(PROTOCOLDIR)/zwlr-layer-shell-v1-protocol.c $(PROTOCOLDIR)/xdg-shell-protocol.c
H_PROTOCOL_HDR = $(PROTOCOLDIR)/zwlr-layer-shell-v1-client-protocol.h $(PROTOCOLDIR)/xdg-shell-client-protocol.h
PROTOCOL_OBJECTS = $(C_PROTOCOL_SRC:$(PROTOCOLDIR)/%.c=$(OBJDIR)/%.o)

# Target executable
TARGET = bongocat

.PHONY: all clean protocols embed-assets

all: protocols embed-assets $(TARGET)

# Generate protocol files first
protocols: $(C_PROTOCOL_SRC) $(H_PROTOCOL_HDR)

# Generate embedded assets
embed-assets: $(EMBEDDED_ASSETS_H) $(EMBEDDED_ASSETS_C)

$(EMBEDDED_ASSETS_H) $(EMBEDDED_ASSETS_C): $(EMBED_SCRIPT) assets/*.png
	exec $(EMBED_SCRIPT)

# Create object directory
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Compile source files (depends on protocol headers and embedded assets)
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(H_PROTOCOL_HDR) $(EMBEDDED_ASSETS_H) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile protocol files
$(OBJDIR)/%.o: $(PROTOCOLDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS) $(PROTOCOL_OBJECTS)
	$(CC) $(OBJECTS) $(PROTOCOL_OBJECTS) -o $(TARGET) $(LDFLAGS)

# Rule to generate Wayland protocol files
$(C_PROTOCOL_SRC) $(H_PROTOCOL_HDR): $(PROTOCOLDIR)/wlr-layer-shell-unstable-v1.xml
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml \
     $(PROTOCOLDIR)/xdg-shell-client-protocol.h
	wayland-scanner private-code   $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml \
     $(PROTOCOLDIR)/xdg-shell-protocol.c
	wayland-scanner private-code $(PROTOCOLDIR)/wlr-layer-shell-unstable-v1.xml $(PROTOCOLDIR)/zwlr-layer-shell-v1-protocol.c
	wayland-scanner client-header $(PROTOCOLDIR)/wlr-layer-shell-unstable-v1.xml $(PROTOCOLDIR)/zwlr-layer-shell-v1-client-protocol.h

clean:
	rm -rf $(OBJDIR) $(TARGET) $(C_PROTOCOL_SRC) $(H_PROTOCOL_HDR) $(EMBEDDED_ASSETS_H) $(EMBEDDED_ASSETS_C)

# Development targets
debug:
	$(MAKE) BUILD_TYPE=debug

release:
	$(MAKE) BUILD_TYPE=release

install: $(TARGET)
	install -D $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	install -D bongocat.conf $(DESTDIR)/usr/local/share/bongocat/bongocat.conf.example

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)
	rm -rf $(DESTDIR)/usr/local/share/bongocat

# Static analysis
analyze:
	clang-tidy $(SOURCES) -- $(CFLAGS)

# Memory check (requires valgrind)
memcheck: debug
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET)

# Performance profiling
profile: release
	perf record -g ./$(TARGET)
	perf report

.PHONY: debug release install uninstall analyze memcheck profile
