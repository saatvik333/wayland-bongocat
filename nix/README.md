# Nix support

- `default.nix` - package derivation (release build, unit tests in the check phase)
- `common.nix` - shared implementation of both modules
- `nixos-module.nix` - NixOS module (`nixosModules.default`)
- `home-module.nix` - Home Manager module (`homeManagerModules.default`)
- `shell.nix` - development shell (`devShells.default`)

From the repository root:

```bash
nix develop                    # development shell
nix build                      # build into ./result
nix run .#default              # build and run
./scripts/test_nix_build.sh    # flake check, package build, dev shell
```

Module settings default to `null` and are omitted from the generated config, so
the binary's defaults apply. Input devices are detected automatically unless
`inputDevices` or `inputDeviceNames` is set. See [NIXOS.md](NIXOS.md) for
module usage, `followActiveMonitor` and `monitorSettings`.
