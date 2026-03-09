# NixOS module for wayland-bongocat
{
  config,
  lib,
  pkgs,
  ...
}:
with lib; let
  cfg = config.programs.wayland-bongocat;
in {
  imports = [./common.nix];
  config = lib.mkIf cfg.enable (let
    configFile = config._bongocat.configFile;
  in {
    environment.systemPackages = [
      cfg.package

      # Helper scripts
      # For starting `wayland-bongocat` using the config file defined with Nix
      (pkgs.writeScriptBin "bongocat-exec" ''
        #!${pkgs.bash}/bin/bash
        exec ${cfg.package}/bin/bongocat --config ${configFile}
      '')
    ];

    # SystemD service
    systemd.user.services.wayland-bongocat = mkIf cfg.autostart {
      enable = true;
      description = "Wayland Bongo Cat Overlay";
      wantedBy = ["graphical-session.target"];
      partOf = ["graphical-session.target"];
      after = ["graphical-session.target"];
      serviceConfig = {
        Type = "exec";
        ExecStart = "${cfg.package}/bin/bongocat --config ${configFile}";
        Restart = "on-failure";
        RestartSec = "5s";
      };
    };
  });
}
