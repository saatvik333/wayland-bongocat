{
  lib,
  config,
  pkgs,
  ...
}: let
  inherit (config._bongocat) cfg configFile;
in {
  imports = [./common.nix];
  config = lib.mkIf cfg.enable {
    home.packages = [
      cfg.package

      # Helper scripts
      # For starting `wayland-bongocat` using the config file defined with Nix
      (pkgs.writeScriptBin "bongocat-exec" ''
        #!${pkgs.bash}/bin/bash
        exec ${cfg.package}/bin/bongocat --config ${configFile}
      '')
    ];

    # SystemD service
    systemd.user.services.wayland-bongocat = lib.mkIf cfg.autostart {
      Unit = {
        Description = "Wayland Bongo Cat Overlay";
        PartOf = ["graphical-session.target"];
        After = ["graphical-session.target"];
      };

      Install = {
        WantedBy = ["graphical-session.target"];
      };

      Service = {
        Type = "exec";
        ExecStart = "${cfg.package}/bin/bongocat --config ${configFile}";
        Restart = "on-failure";
        RestartSec = "5s";
      };
    };
  };
}
