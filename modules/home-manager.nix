{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.chatterinoTabEmit;

  pluginBase =
    if cfg.flatpak then
      ".var/app/com.chatterino.chatterino/data/chatterino/Plugins"
    else
      ".local/share/chatterino/Plugins";

  tabEmitTarget = "${pluginBase}/tab-emit";

  streamlinkPackage = config.programs.streamlink.package or pkgs.streamlink;

  # nixpkgs exposes Streamlink as a Python application. Convert the configured
  # package back into an importable module for python.withPackages.
  streamlinkPythonModule = pkgs.python3.pkgs.toPythonModule streamlinkPackage;
  pythonWithStreamlink = pkgs.python3.withPackages (_: [ streamlinkPythonModule ]);

  rumbleStreamlinkPlugin = pkgs.runCommand "streamlink-rumble-plugin" { } ''
    install -Dm644 ${self}/streamlink-plugins/rumble.py \
      "$out/share/streamlink/plugins/rumble.py"
  '';

  listener = pkgs.writeShellApplication {
    name = "chatterino-tabemit-listener";
    runtimeInputs = [ pkgs.python3 ] ++ lib.optionals cfg.enableNotify [ pkgs.libnotify ];
    text = ''
      exec ${pkgs.python3}/bin/python3 ${self}/tabemit-listener.py "$@"
    '';
  };

  streamlinkFollower = pkgs.writeShellApplication {
    name = "chatterino-streamlink-follow";
    runtimeInputs = [ pythonWithStreamlink streamlinkPackage pkgs.mpv ];
    text = ''
      export CHATTERINO_STREAMLINK_PLUGIN_DIR=${rumbleStreamlinkPlugin}/share/streamlink/plugins
      exec ${pythonWithStreamlink}/bin/python3 ${self}/chatterino-streamlink-follow-fixed.py --mpv-arg "--title=streamlink - mpv" "$@"
    '';
  };

  streamlinkSession = pkgs.writeShellApplication {
    name = "chatterino-streamlink-session";
    runtimeInputs = [ streamlinkFollower ];
    text = ''
      set -euo pipefail

      chatterino_cmd=''${CHATTERINO_TABEMIT_CHATTERINO:-chatterino}
      follower_args=()

      while [ "$#" -gt 0 ]; do
        case "$1" in
          --)
            shift
            break
            ;;
          *)
            follower_args+=("$1")
            shift
            ;;
        esac
      done

      chatterino-streamlink-follow "''${follower_args[@]}" &
      follower_pid=$!

      cleanup() {
        kill "$follower_pid" 2>/dev/null || true
        wait "$follower_pid" 2>/dev/null || true
      }
      trap cleanup EXIT INT TERM

      sleep 0.3
      "$chatterino_cmd" "$@" &
      chatterino_pid=$!
      wait "$chatterino_pid"
    '';
  };
in
{
  options.programs.chatterinoTabEmit = {
    enable = lib.mkEnableOption "Chatterino tab switch emitter plugin";

    flatpak = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Install the plugin into Chatterino's Flatpak app-data directory instead
        of the normal ~/.local/share/chatterino directory.
      '';
    };

    installListener = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Install the chatterino-tabemit-listener command into home.packages.";
    };

    installStreamlinkFollower = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Install the chatterino-streamlink-follow command into home.packages.";
    };

    installStreamlinkSession = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Install the chatterino-streamlink-session launcher command into home.packages.";
    };

    enableStreamlinkFollowerService = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Run chatterino-streamlink-follow as a Home Manager systemd user service.";
    };

    streamlinkFollowerServiceArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      example = [ "--quality" "720p60" ];
      description = "Arguments passed to the chatterino-streamlink-follow user service.";
    };

    enableNotify = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Include libnotify so chatterino-tabemit-listener --notify works.";
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    {
      home.file."${tabEmitTarget}/init.lua".source = "${self}/tab-emit/init.lua";
      home.file."${tabEmitTarget}/info.json".source = "${self}/tab-emit/info.json";
    }

    (lib.mkIf cfg.installListener {
      home.packages = [ listener ];
    })

    (lib.mkIf cfg.installStreamlinkFollower {
      home.packages = [ streamlinkFollower ];
    })

    (lib.mkIf cfg.installStreamlinkSession {
      home.packages = [ streamlinkSession ];
    })

    (lib.mkIf cfg.enableStreamlinkFollowerService {
      systemd.user.services.chatterino-streamlink-follow = {
        Unit = {
          Description = "Follow Chatterino tabs with Streamlink/mpv";
          After = [ "graphical-session.target" ];
        };
        Service = {
          ExecStart = "${streamlinkFollower}/bin/chatterino-streamlink-follow ${lib.escapeShellArgs cfg.streamlinkFollowerServiceArgs}";
          Restart = "on-failure";
          RestartSec = 2;
        };
        Install = {
          WantedBy = [ "graphical-session.target" ];
        };
      };
    })
  ]);
}
