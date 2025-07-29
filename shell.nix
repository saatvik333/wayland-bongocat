{
  pkgs ? import <nixpkgs> {},
  clangStdenv ? import <nixpkgs> {},
  stdenv ? import <nixpkgs> {},
}:
clangStdenv.mkDerivation {
  name = "nix-shell";
  phases = ["nobuildPhase"];
  buildInputs = with pkgs; [
    xxd
    wayland
    wayland-scanner
    wayland-protocols
    pkg-config
  ];

  nobuildPhase = ''
    echo
    echo "This derivation is not meant to be built, aborting";
    echo
    exit 1
  '';
}
