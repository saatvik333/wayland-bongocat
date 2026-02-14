# Nix/NixOS Support
## Files
- **`NIXOS.md`** - Comprehensive installation guide for Nix/NixOS users
- **`default.nix`** - Main package derivation
- **`nixos-module.nix`** - NixOS system module
- **`shell.nix`** - Development environment
- **`scripts/test_nix_build.sh`** - Test script for validating builds

## Quick Usage
Run these commands from the root directory of the project where `flake.nix` is.
```bash
# Enter development shell
nix develop

# Build
nix build

# Build and run
nix run ./#default

# Test all Nix builds
./scripts/test_nix_build.sh
```

See [NIXOS.md](NIXOS.md) for further information.
