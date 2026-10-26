#!/usr/bin/env bash
# Check the Nix flake, package build and development shell.
set -euo pipefail

command -v nix >/dev/null || { echo "error: nix is required" >&2; exit 1; }

echo "Checking flake..."
nix flake check --no-build

echo "Building package..."
nix build --no-link

echo "Entering development shell..."
nix develop --command true

echo "All Nix checks passed."
