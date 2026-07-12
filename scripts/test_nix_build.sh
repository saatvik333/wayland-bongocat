#!/usr/bin/env bash
# Test script for Nix builds
set -euo pipefail

echo "🧪 Testing Nix builds for wayland-bongocat"
echo "=========================================="

command -v nix >/dev/null || { echo "❌ Nix is required"; exit 1; }

# Test flake build
echo "📦 Testing flake build..."
nix flake check --no-build
echo "✅ Flake check: SUCCESS"
nix build --no-link
echo "✅ Flake build: SUCCESS"

# Test development shell
echo ""
echo "🔧 Testing development shell..."
nix-shell nix/shell.nix --run "echo 'Shell works'" >/dev/null
echo "✅ Development shell: SUCCESS"

echo ""
echo "🎉 All Nix builds completed successfully!"
echo ""
