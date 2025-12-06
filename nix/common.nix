{
  lib,
  config,
  pkgs,
  ...
}: let
  cfg = config.programs.wayland-bongocat;
  wayland-bongocat = pkgs.callPackage ./default.nix {};
  configFile = pkgs.writeTextFile {
    name = "bongocat.conf";
    text = ''
      # Auto-generated config for `wayland-bongocat`

      # Cat position and size
      cat_x_offset=${toString cfg.catXOffset}
      cat_y_offset=${toString cfg.catYOffset}
      cat_height=${toString cfg.catHeight}
      cat_align=${cfg.catAlign}

      # Visual settings
      mirror_x=${if cfg.mirrorX then "1" else "0"}
      mirror_y=${if cfg.mirrorY then "1" else "0"}
      enable_antialiasing=${if cfg.enableAntialiasing then "1" else "0"}

      # Overlay settings
      overlay_position=${cfg.overlayPosition}
      overlay_height=${toString cfg.overlayHeight}
      overlay_opacity=${toString cfg.overlayOpacity}
      layer=${cfg.layer}

      # Animation settings
      idle_frame=${toString cfg.idleFrame}
      keypress_duration=${toString cfg.keypressDuration}
      test_animation_duration=${toString cfg.testAnimationDuration}
      test_animation_interval=${toString cfg.testAnimationInterval}
      fps=${toString cfg.fps}

      # Sleep mode
      idle_sleep_timeout=${toString cfg.idleSleepTimeout}
      enable_scheduled_sleep=${if cfg.enableScheduledSleep then "1" else "0"}
      sleep_begin=${cfg.sleepBegin}
      sleep_end=${cfg.sleepEnd}

      # Debug mode
      enable_debug=${if cfg.enableDebug then "1" else "0"}

      # Monitor
      monitor=${cfg.monitor}

      # Input devices
      ${lib.concatMapStringsSep "\n" (device: "keyboard_device=${device}") cfg.inputDevices}
    '';
  };
in {
  meta.maintainers = with lib.maintainers; [];
  options.programs.wayland-bongocat = {
    enable = lib.mkOption {
      type = lib.types.bool;
      default = false;
      example = true;
      description = "Enable `wayland-bongocat` overlay";
    };
    autostart = lib.mkOption {
      type = lib.types.bool;
      default = false;
      example = true;
      description = "Enable and automatically start `bongocat-wayland` as a service on login";
    };

    package = lib.mkOption {
      type = lib.types.package;
      default = wayland-bongocat;
      description = "The wayland-bongocat package to use.";
    };

    # Debug mode
    enableDebug = lib.mkOption {
      type = lib.types.bool;
      default = false;
      example = true;
      description = "Enable debug logging";
    };

    # Overlay
    overlayPosition = lib.mkOption {
      type = lib.types.str;
      default = "top";
      example = "bottom";
      description = "Bongocat overlay position on screen - `top` or `bottom`";
    };
    overlayHeight = lib.mkOption {
      type = lib.types.int;
      default = 60;
      example = 70;
      description = "Height of the entire overlay bar";
    };
    overlayOpacity = lib.mkOption {
      type = lib.types.int;
      default = 0;
      example = 20;
      description = "Overlay background opacity (0-255, 0 = transparent)";
    };

    # Position
    catXOffset = lib.mkOption {
      type = lib.types.int;
      default = 120;
      example = 200;
      description = "Horizontal offset from center position (pixels)";
    };
    catYOffset = lib.mkOption {
      type = lib.types.int;
      default = 0;
      example = 10;
      description = "Vertical offset from center position (pixels)";
    };

    # Size
    catHeight = lib.mkOption {
      type = lib.types.int;
      default = 80;
      example = 50;
      description = "Height of the bongo cat in pixels";
    };
    catAlign = lib.mkOption {
      type = lib.types.str;
      default = "center";
      example = "right";
      description = "Horizontal alignment: left, center, or right";
    };

    # Visual settings
    mirrorX = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Flip cat horizontally";
    };
    mirrorY = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Flip cat vertically";
    };
    enableAntialiasing = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Enable smooth scaling (recommended)";
    };
    layer = lib.mkOption {
      type = lib.types.str;
      default = "top";
      example = "overlay";
      description = "Layer type: top or overlay";
    };

    # Sleep mode
    idleSleepTimeout = lib.mkOption {
      type = lib.types.int;
      default = 0;
      example = 300;
      description = "Seconds of inactivity before sleep (0 = disabled)";
    };
    enableScheduledSleep = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Enable scheduled sleep mode";
    };
    sleepBegin = lib.mkOption {
      type = lib.types.str;
      default = "22:00";
      description = "Sleep schedule start time (HH:MM)";
    };
    sleepEnd = lib.mkOption {
      type = lib.types.str;
      default = "06:00";
      description = "Sleep schedule end time (HH:MM)";
    };

    # Animations
    idleFrame = lib.mkOption {
      type = lib.types.int;
      default = 0;
      example = 1;
      description = "Frame to use when idle (0, 1, or 2)";
    };
    keypressDuration = lib.mkOption {
      type = lib.types.int;
      default = 150;
      example = 100;
      description = "Animation duration after keypress (ms)";
    };
    testAnimationDuration = lib.mkOption {
      type = lib.types.int;
      default = 200;
      example = 100;
      description = "Test animation duration (ms)";
    };
    testAnimationInterval = lib.mkOption {
      type = lib.types.int;
      default = 0;
      example = 10;
      description = "How often to trigger test animation (seconds, 0 = disabled)";
    };

    # Performance
    fps = lib.mkOption {
      type = lib.types.int;
      default = 60;
      example = 120;
      description = "Animation framerate (FPS)";
    };

    # Input devices
    inputDevices = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = ["/dev/input/event4"];
      description = "List of input devices to monitor, run `bongocat-find-devices` to see all devices to add to this list";
      example = [
        "/dev/input/event4"
        "/dev/input/event20"
      ];
    };

    monitor = lib.mkOption {
      type = lib.types.str;
      default = "";
      description = "The monitor for the Cat";
      example = "eDP-1";
    };
  };

  # Configuration
  config._bongocat = {
    inherit cfg configFile;
  };
}
