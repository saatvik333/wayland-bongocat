# Nix and NixOS

## Running without a module

```bash
nix run github:saatvik333/wayland-bongocat
nix profile install github:saatvik333/wayland-bongocat
```

Keyboards are detected automatically. `bongocat --list-devices` lists input
devices with their stable `/dev/input/by-id/` paths and a suggested
`keyboard_name=` line if you want to pin specific devices.

## NixOS module

```nix
# flake.nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    bongocat.url = "github:saatvik333/wayland-bongocat";
  };

  outputs = inputs: {
    nixosConfigurations.your-hostname = inputs.nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        inputs.bongocat.nixosModules.default
        {
          programs.wayland-bongocat = {
            enable = true;
            autostart = true;
          };
        }
      ];
    };
  };
}
```

Without flakes, import `/path/to/wayland-bongocat/nix/nixos-module.nix`.

## Home Manager module

The same options are available through `homeManagerModules.default`
(`homeModule.default` is an alias):

```nix
inputs.home-manager.lib.homeManagerConfiguration {
  # ...
  modules = [
    inputs.bongocat.homeManagerModules.default
    { programs.wayland-bongocat.enable = true; }
  ];
}
```

Without flakes, import `/path/to/wayland-bongocat/nix/home-module.nix`.

## Options

All options live under `programs.wayland-bongocat`.

| Option | Description |
| --- | --- |
| `enable` | Install `bongocat` and the `bongocat-exec` helper, which runs it with the generated config. |
| `autostart` | Run bongocat as the systemd user service `wayland-bongocat`, bound to `graphical-session.target`. |
| `package` | Package to use. |
| `monitor` | Comma-separated output names. Empty (default) selects the output automatically. |
| `followActiveMonitor` | With automatic selection, move the overlay to the focused output. |
| `inputDevices` | Explicit `/dev/input/...` paths. Empty with no `inputDeviceNames` means automatic detection. |
| `inputDeviceNames` | Device name substrings to match. |
| `monitorSettings` | Per-output appearance overrides (see below). |
| `extraConfig` | Raw lines appended to the global section of `bongocat.conf`. |

Appearance: `catHeight`, `catXOffset`, `catYOffset`, `catAlign`, `mirrorX`,
`mirrorY`, `overlayHeight`, `overlayOpacity`, `overlayPosition`, `layer`,
`disableFullscreenHide`.

Behaviour: `fps`, `keypressDuration`, `idleFrame`, `testAnimationDuration`,
`testAnimationInterval`, `enableHandMapping`, `enableScheduledSleep`,
`sleepBegin`, `sleepEnd` (`HH:MM`), `idleSleepTimeout`, `enableDebug`.

These settings default to `null`. Unset settings are not written to the
generated config, so the binary's built-in defaults apply (see `man bongocat`
or the option descriptions). Ranges are checked at evaluation time.

### Per-monitor overrides

`monitorSettings` maps an output name to appearance overrides using the
snake_case config keys. Unset keys inherit the global value. Overrides do not
select outputs; use `monitor` for that.

```nix
programs.wayland-bongocat = {
  monitor = "eDP-1,HDMI-A-1";
  catHeight = 50;
  monitorSettings."HDMI-A-1" = {
    cat_height = 60;
    cat_align = "right";
    mirror_x = true;
  };
};
```

## Service management

```bash
systemctl --user status wayland-bongocat
systemctl --user restart wayland-bongocat
journalctl --user -u wayland-bongocat -f
```

## Troubleshooting

- Input permission errors: add yourself to the `input` group
  (`users.users.<name>.extraGroups = [ "input" ];`) and log in again.
- `bongocat --config <file> --check-config` validates a config without a
  Wayland session; `bongocat --doctor` checks compositor protocols and input access.
- `bongocat --list-devices` shows which devices are detected.
- Set `enableDebug = true;` and read the service logs.

Only one bongocat instance runs per session.

## Development

```bash
nix develop                    # toolchain, wayland-scanner, python3, clang-tools
nix build                      # package in ./result (runs the unit tests)
./scripts/test_nix_build.sh    # flake check, package build, dev shell
```
