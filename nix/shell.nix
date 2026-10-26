{pkgs ? import <nixpkgs> {}}:
pkgs.mkShellNoCC {
  nativeBuildInputs = with pkgs; [
    gcc # Compiler (make is provided by the shell's stdenv)
    wayland-scanner # make protocols, make test-runtime
    python3 # make test-runtime
    clang-tools # clang-format, clang-tidy (make format-check, make lint)
    gdb

    # Input device debugging
    udev
    evtest
  ];
  buildInputs = with pkgs; [
    wayland # libwayland-client; libwayland-server for make test-runtime
    wayland-protocols
  ];
  shellHook = ''
    echo "wayland-bongocat development shell"
    echo "  make / make release  optimized build (build/release/bongocat)"
    echo "  make debug           ASan + UBSan build"
    echo "  make test            unit tests"
    echo "  make test-sanitize   unit tests under ASan + UBSan"
    echo "  make test-runtime    compositor fixture tests"
    echo "  make format-check    check formatting"
    echo "  make lint            clang-tidy"
    echo "  nix build            build the package into ./result"
    echo "  ./scripts/test_nix_build.sh  check the flake, package and dev shell"
  '';
}
