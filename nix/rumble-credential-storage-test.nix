{ pkgs, probe }:

let
  workingTest = pkgs.writeShellApplication {
    name = "run-rumble-credential-storage-test";
    runtimeInputs = [
      probe
      pkgs.dbus
      pkgs.gnome-keyring
    ];
    text = ''
      umask 077
      export XDG_RUNTIME_DIR="''${XDG_RUNTIME_DIR:-/tmp/rumble-credential-runtime}"
      mkdir -p "$XDG_RUNTIME_DIR"

      printf '%s' 'synthetic-keyring-password' |
        gnome-keyring-daemon --unlock --components=secrets

      # These are deliberately separate processes. They model Chatterino
      # exiting after login and loading the saved account on the next start.
      rumble-credential-store-probe erase
      rumble-credential-store-probe write
      rumble-credential-store-probe read
      rumble-credential-store-probe erase
      rumble-credential-store-probe read-missing
    '';
  };
in
pkgs.testers.runNixOSTest {
  name = "rumble-credential-storage";

  nodes = {
    working =
      { ... }:
      {
        users.users.alice = {
          isNormalUser = true;
          uid = 1000;
        };
        services.gnome.gnome-keyring.enable = true;
        systemd.tmpfiles.rules = [
          "d /tmp/rumble-credential-runtime 0700 alice users -"
        ];
        environment.systemPackages = [
          probe
          workingTest
        ];
      };

    missing =
      { ... }:
      {
        users.users.alice = {
          isNormalUser = true;
          uid = 1000;
        };
        systemd.tmpfiles.rules = [
          "d /tmp/rumble-credential-runtime 0700 alice users -"
        ];
        environment.systemPackages = [
          pkgs.dbus
          probe
        ];
      };
  };

  testScript = ''
    start_all()

    with subtest("write, process restart, read, erase"):
        output = working.succeed(
            "sudo -u alice env HOME=/home/alice "
            "XDG_RUNTIME_DIR=/tmp/rumble-credential-runtime "
            "dbus-run-session -- run-rumble-credential-storage-test"
        )
        assert output.count("result=success") == 5, output
        assert "operation=write" in output, output
        assert "operation=read" in output, output
        assert "operation=read-missing" in output, output

    with subtest("missing credential storage is actionable"):
        output = missing.succeed(
            "sudo -u alice env HOME=/home/alice "
            "XDG_RUNTIME_DIR=/tmp/rumble-credential-runtime "
            "dbus-run-session -- "
            "${probe}/bin/rumble-credential-store-probe expect-unavailable"
        )
        assert "qtkeychain_available=true" in output, output
        assert "session_bus_address_set=true" in output, output
        assert "result=success" in output, output
        assert "store_error=unavailable" in output, output
        assert "system keyring" in output, output
  '';
}
