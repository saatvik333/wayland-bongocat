{pkgs ? import <nixpkgs> {}}:
pkgs.mkShellNoCC {
  nativeBuildInputs = with pkgs; [
    # Build dependencies
    # Core
    pkg-config # Finds build dependencies
    gcc # C/C++ compiler and also for `make`

    # Wayland
    wayland-scanner

    # Devtools
    gdb # Debugger
    valgrind # Memory debugger
    clang-tools # Useful tools for C/C++ including a formatter `clang-format`

    # Optional tools for input device debugging
    evtest
    udev
  ];
  buildInputs = with pkgs; [
    wayland
    wayland-protocols
  ];
  shellHook = ''
    # Ensure that the Makefile can find and access the Wayland protocols
    export WAYLAND_PROTOCOLS_DIR="${pkgs.wayland-protocols}/share/wayland-protocols"

    # Simple TUI
    echo "🐱 Bongo Cat Development Environment"
    echo "Build output is stored in 'build' if you don't use 'nix build'"
    echo "Commands:"
    echo "  nix build         - Build the Nix package (Build output is stored in 'result')"
    echo "  make              - Build the project"
    echo "  make debug        - Build with debug symbols"
    echo "  make release      - Build in release mode with optimizations and such (Longer compile time)"
    echo "  make protocols    - Generate protocol files"
    echo "  make embed-assets - Generate embedded assets"
    echo "  make clean        - Clean build artifacts"
    echo "  make memcheck     - Run with valgrind (Requires debug build)"
    echo ""
    echo "Helper scripts:"
    echo "  ./scripts/find_input_devices.sh - Find input devices"
    echo "  ./scripts/test-nix-build.sh     - Test Nix flake and package"
    echo "  ./scripts/test_toggle.sh        - Test Bongocat toggle functionality (Install Bongocat first)"
  '';
}
