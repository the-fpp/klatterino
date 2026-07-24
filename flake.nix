{
  description = "Chatterino plugins + chatterino packaged for Home Manager";

  inputs = {
    self.submodules = true;
    # Keep the standalone validation package reproducible without relying on a
    # mutable registry alias. Update this revision intentionally.
    nixpkgs.url = "github:NixOS/nixpkgs/18b9261cb3294b6d2a06d03f96872827b8fe2698";
    nix-appimage = {
      url = "github:ralismark/nix-appimage";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      nix-appimage,
    }:
    let
      # Hash only the vendored application subtree. Changes to documentation or
      # the UI controller must not trigger a full Chatterino rebuild.
      chatterino7Src = builtins.path {
        path = ./deps/chatterino7;
        name = "chatterino7-source";
      };
      submoduleSentinel = "${chatterino7Src}/lib/signals/CMakeLists.txt";
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      chatterinoOverlay = final: prev: {
        chatterino7 = prev.chatterino7.overrideAttrs (old: {
          src =
            if builtins.pathExists submoduleSentinel then
              chatterino7Src
            else
              throw ''
                klatterino was fetched without vendored Chatterino submodules.

                This flake declares inputs.self.submodules = true, so update the
                locked input with a Nix version that supports flake self-attributes.

                Workaround for older Nix:
                  git+https://github.com/the-fpp/klatterino.git?submodules=1

                or locally run:
                  git submodule update --init --recursive
              '';

          # The Chatterino changes are committed directly in deps/chatterino7.
          # Do not also apply the old patch file.
          patches = [ ];
          qtWrapperArgs =
            (old.qtWrapperArgs or [ ])
            ++ [
              "--set-default"
              "CHATTERINO_SELENIUM_MANAGER"
              "${final.selenium-manager}/bin/selenium-manager"
            ]
            ++ final.lib.optionals final.stdenv.hostPlatform.isLinux [
              "--set-default"
              "CHATTERINO_WEBDRIVER_LOADER"
              final.stdenv.cc.bintools.dynamicLinker
              "--set-default"
              "CHATTERINO_WEBDRIVER_LIBRARY_PATH"
              (final.lib.makeLibraryPath [
                final.stdenv.cc.cc.lib
                final.stdenv.cc.libc
                final.dbus
                final.glib
                final.libxcb
                final.nspr
                final.nss
              ])
            ];
        });
      };
      validationFor =
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          streamlinkPythonModule = pkgs.python3.pkgs.toPythonModule pkgs.streamlink;
          pythonWithStreamlink = pkgs.python3.withPackages (_: [ streamlinkPythonModule ]);
          mkRunner =
            name: command:
            pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = [ pkgs.python3 ];
              text = ''
                exec python3 ${self}/tools/rumble_validation.py ${command} "$@"
              '';
            };
          public = mkRunner "rumble-validate-public" "public";
          diagnose = mkRunner "rumble-diagnose-channel" "diagnose";
          authenticated = mkRunner "rumble-validate-authenticated" "authenticated";
          selfTest = pkgs.writeShellApplication {
            name = "rumble-validation-self-test";
            runtimeInputs = [ pkgs.python3 ];
            text = ''
              export PYTHONDONTWRITEBYTECODE=1
              export PYTHONPATH=${self}
              exec python3 -B -m unittest -v \
                tools.tests.test_rumble_validation
            '';
          };
          streamlinkPlugin = pkgs.runCommand "streamlink-rumble-plugin" { } ''
            install -Dm644 ${self}/streamlink-plugins/rumble.py \
              "$out/share/streamlink/plugins/rumble.py"
          '';
          streamlinkSelfTest = pkgs.writeShellApplication {
            name = "rumble-streamlink-self-test";
            runtimeInputs = [ pythonWithStreamlink ];
            text = ''
              export PYTHONDONTWRITEBYTECODE=1
              export PYTHONPATH=${self}
              exec ${pythonWithStreamlink}/bin/python3 -B -m unittest -v \
                tools.tests.test_rumble_streamlink \
                tools.tests.test_rumble_streamlink_discovery
            '';
          };
          aggregate = pkgs.symlinkJoin {
            name = "rumble-validation-${builtins.substring 0 7 (self.rev or "dirty")}";
            paths = [
              public
              diagnose
              authenticated
              selfTest
              streamlinkSelfTest
              streamlinkPlugin
            ];
          };
          check =
            pkgs.runCommand "rumble-validation-self-test"
              {
                nativeBuildInputs = [ pythonWithStreamlink ];
              }
              ''
                export PYTHONDONTWRITEBYTECODE=1
                export PYTHONPATH=${self}
                ${pythonWithStreamlink}/bin/python3 -B -m unittest -v \
                  tools.tests.test_rumble_validation \
                  tools.tests.test_rumble_streamlink \
                  tools.tests.test_rumble_streamlink_discovery
                touch "$out"
              '';
        in
        {
          inherit
            public
            diagnose
            authenticated
            selfTest
            streamlinkPlugin
            streamlinkSelfTest
            aggregate
            check
            ;
        };
      uiFor =
        system: withChatterino:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ chatterinoOverlay ];
          };
          tesseractEnglish = pkgs.tesseract.override {
            enableLanguages = [ "eng" ];
          };
        in
        pkgs.writeShellApplication {
          name = "chatterino-ui-xephyr";
          runtimeInputs = [
            pkgs.coreutils
            pkgs.gnugrep
            pkgs.gnused
            pkgs.imagemagick
            pkgs.jq
            pkgs.openbox
            tesseractEnglish
            pkgs.util-linux
            pkgs.xdotool
            pkgs.xauth
            pkgs.xdpyinfo
            pkgs.xorg-server
          ];
          text =
            nixpkgs.lib.optionalString withChatterino ''
              export CHATTERINO_UI_DEFAULT_APP=${pkgs.chatterino7}/bin/chatterino
            ''
            + builtins.readFile ./tools/ui/chatterino-xephyr;
        };
      uiControllerFor = system: uiFor system false;
      uiHarnessFor = system: uiFor system true;
      credentialStoreProbeFor =
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        pkgs.stdenv.mkDerivation {
          pname = "rumble-credential-store-probe";
          version = "1";
          src = chatterino7Src;
          cmakeDir = "../tests/credential-store-probe";
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.qt6.wrapQtAppsHook
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6Packages.qtkeychain
          ];
        };
      credentialStoreTestFor =
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        import ./nix/rumble-credential-storage-test.nix {
          inherit pkgs;
          probe = credentialStoreProbeFor system;
        };
      portableWindowsFor =
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config = {
              allowBroken = true;
              allowUnsupportedSystem = true;
            };
          };
        in
        import ./nix/portable-windows.nix {
          inherit pkgs chatterino7Src;
          revision = self.dirtyRev or self.rev or "dirty";
          nixpkgsRevision = nixpkgs.rev;
        };
      portableLinuxRuntimeProbeFor =
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        pkgs.stdenv.mkDerivation {
          pname = "chatterino-portable-runtime-probe";
          version = "1";
          src = ./nix/portable-linux-runtime-probe.cpp;
          dontUnpack = true;
          dontWrapQtApps = true;
          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtwebsockets
          ];
          buildPhase = ''
            runHook preBuild
            $CXX -std=c++20 -O2 -Wall -Wextra -Werror \
              "${./nix/portable-linux-runtime-probe.cpp}" \
              "${chatterino7Src}/src/common/network/CertificateBundle.cpp" \
              -I"${chatterino7Src}/src" \
              -o chatterino-portable-runtime-probe \
              $(pkg-config --cflags --libs Qt6Core Qt6Network Qt6WebSockets)
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 chatterino-portable-runtime-probe \
              "$out/bin/chatterino-portable-runtime-probe"
            runHook postInstall
          '';
        };
      portableLinuxFor =
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ chatterinoOverlay ];
          };
          revision = self.dirtyRev or self.rev or "dirty";
          shortRevision =
            builtins.substring 0 12 revision
            + nixpkgs.lib.optionalString (nixpkgs.lib.hasSuffix "-dirty" revision) "-dirty";
          filename = "chatterino-${shortRevision}-${system}.AppImage";
          runtimeProbe = portableLinuxRuntimeProbeFor system;
          portableLauncher = pkgs.writeShellApplication {
            name = "chatterino";
            runtimeInputs = [
              pkgs.coreutils
              pkgs.fontconfig.bin
              pkgs.procps
            ];
            text = ''
              certificate="''${CHATTERINO_SSL_CERT_FILE:-${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt}"
              fontconfig="''${CHATTERINO_FONTCONFIG_FILE:-${pkgs.fontconfig.out}/etc/fonts/fonts.conf}"

              export CHATTERINO_SSL_CERT_FILE="$certificate"
              export SSL_CERT_FILE="$certificate"
              export NIX_SSL_CERT_FILE="$certificate"
              unset SSL_CERT_DIR
              export FONTCONFIG_FILE="$fontconfig"
              FONTCONFIG_PATH="''${fontconfig%/*}"
              export FONTCONFIG_PATH

              if [[ "''${1-}" == "--portable-runtime-probe" ]]; then
                shift
                test -s "$SSL_CERT_FILE"
                fc-match sans-serif >/dev/null
                pgrep --version >/dev/null
                echo "portable-ca-bundle=ok"
                echo "portable-fontconfig=ok"
                echo "portable-pgrep=ok"
                exec ${runtimeProbe}/bin/chatterino-portable-runtime-probe "$@"
              fi

              exec ${pkgs.chatterino7}/bin/chatterino "$@"
            '';
          };
          # Keep the desktop file and icon visible to nix-appimage's metadata
          # collector while making the portable launcher the image entrypoint.
          portableEntrypoint = pkgs.symlinkJoin {
            name = "chatterino-portable-entrypoint";
            paths = [ portableLauncher ];
            postBuild = ''
              ln -s ${pkgs.chatterino7}/share "$out/share"
            '';
          };
          appImage = nix-appimage.lib.${system}.mkAppImage {
            program = "${portableEntrypoint}/bin/chatterino";
            pname = "chatterino";
            name = filename;
            squashfsArgs = [
              "-no-xattrs"
            ];
          };
          manifest = pkgs.writeText "chatterino-linux-manifest.json" (
            builtins.toJSON {
              artifact = filename;
              format = "AppImage type 2";
              inherit revision system;
              nixpkgs = nixpkgs.rev;
              nixAppImage = nix-appimage.rev or "locked";
              seleniumManager = pkgs.selenium-manager.version;
              loginBrowserPolicy = "installed system browser; no bundled browser engine";
              caBundle = pkgs.cacert.name;
              fontconfig = pkgs.fontconfig.version;
              processHelper = "procps-${pkgs.procps.version}";
              runtimeProbe = "Qt HTTPS/WSS, fontconfig, and pgrep";
            }
          );
        in
        pkgs.runCommand "chatterino-linux-appimage-${shortRevision}"
          {
            nativeBuildInputs = [ pkgs.coreutils ];
          }
          ''
            mkdir -p "$out/licenses"
            install -Dm755 ${appImage} "$out/${filename}"
            install -Dm644 ${manifest} "$out/manifest.json"
            install -Dm644 ${chatterino7Src}/LICENSE "$out/licenses/Chatterino.txt"
            install -Dm644 ${./docs/THIRD-PARTY-NOTICES.md} \
              "$out/licenses/THIRD-PARTY-NOTICES.md"
            install -Dm644 ${chatterino7Src}/lib/expected-lite/LICENSE.txt \
              "$out/licenses/expected-lite.txt"
            install -Dm644 ${chatterino7Src}/lib/libcommuni/LICENSE \
              "$out/licenses/LibCommuni.txt"
            install -Dm644 ${chatterino7Src}/lib/lrucache/LICENSE \
              "$out/licenses/LRUCache.txt"
            install -Dm644 ${chatterino7Src}/lib/magic_enum/LICENSE \
              "$out/licenses/magic-enum.txt"
            install -Dm644 ${chatterino7Src}/lib/miniaudio/LICENSE \
              "$out/licenses/miniaudio.txt"
            install -Dm644 ${chatterino7Src}/lib/rapidjson/license.txt \
              "$out/licenses/RapidJSON.txt"
            install -Dm644 ${chatterino7Src}/lib/serialize/LICENSE \
              "$out/licenses/pajlada-serialize.txt"
            install -Dm644 ${chatterino7Src}/lib/settings/LICENSE \
              "$out/licenses/pajlada-settings.txt"
            install -Dm644 ${chatterino7Src}/lib/signals/LICENSE \
              "$out/licenses/pajlada-signals.txt"
            install -Dm644 ${chatterino7Src}/lib/sol2/LICENSE.txt \
              "$out/licenses/sol2.txt"
            install -Dm644 \
              ${chatterino7Src}/lib/twitch-eventsub-ws/lib/date/LICENSE.txt \
              "$out/licenses/date.txt"
            install -Dm644 \
              ${chatterino7Src}/lib/twitch-eventsub-ws/lib/fmt/LICENSE \
              "$out/licenses/fmt.txt"
            (
              cd "$out"
              sha256sum "${filename}" manifest.json licenses/* \
                > SHA256SUMS
              sha256sum --check SHA256SUMS
            )
          '';
      portableLinuxRuntimeCheckFor =
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          artifact = portableLinuxFor system;
          runtimeProbe = portableLinuxRuntimeProbeFor system;
          runtimeProbeClosure = pkgs.closureInfo {
            rootPaths = [
              runtimeProbe
              pkgs.cacert
              pkgs.fontconfig.bin
              pkgs.fontconfig.out
              pkgs.procps
            ];
          };
        in
        pkgs.runCommand "chatterino-linux-appimage-runtime-check"
          {
            nativeBuildInputs = [
              pkgs.coreutils
              pkgs.gnugrep
              pkgs.gnused
              pkgs.openssl
              pkgs.proot
              pkgs.python3
              pkgs.squashfsTools
            ];
          }
          ''
            image="$(find ${artifact} -maxdepth 1 -name '*.AppImage' -print -quit)"
            test -n "$image"
            test -x "$image"

            mkdir -p "$TMPDIR/home"
            export HOME="$TMPDIR/home"
            export QT_QPA_PLATFORM=offscreen

            openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
              -subj '/CN=Chatterino portable runtime test CA' \
              -addext 'basicConstraints=critical,CA:TRUE' \
              -addext 'keyUsage=critical,keyCertSign,cRLSign' \
              -keyout "$TMPDIR/ca.key" \
              -out "$TMPDIR/ca.crt" >/dev/null 2>&1
            openssl req -newkey rsa:2048 -nodes \
              -subj '/CN=127.0.0.1' \
              -addext 'subjectAltName=IP:127.0.0.1' \
              -addext 'basicConstraints=critical,CA:FALSE' \
              -addext 'keyUsage=critical,digitalSignature,keyEncipherment' \
              -addext 'extendedKeyUsage=serverAuth' \
              -keyout "$TMPDIR/server.key" \
              -out "$TMPDIR/server.csr" >/dev/null 2>&1
            openssl x509 -req -days 1 \
              -in "$TMPDIR/server.csr" \
              -CA "$TMPDIR/ca.crt" \
              -CAkey "$TMPDIR/ca.key" \
              -CAcreateserial \
              -copy_extensions copy \
              -out "$TMPDIR/server.crt" >/dev/null 2>&1
            python3 ${./nix/portable-linux-runtime-test-server.py} \
              --certificate "$TMPDIR/server.crt" \
              --key "$TMPDIR/server.key" \
              --ready-file "$TMPDIR/runtime-server" &
            server_pid=$!
            trap 'kill "$server_pid" 2>/dev/null || true' EXIT
            for _ in $(seq 1 100); do
              test ! -s "$TMPDIR/runtime-server" || break
              sleep 0.05
            done
            test -s "$TMPDIR/runtime-server"
            read -r https_port wss_port < "$TMPDIR/runtime-server"
            probe_args=(
              --portable-runtime-probe
              --https-url "https://127.0.0.1:$https_port/"
              --wss-url "wss://127.0.0.1:$wss_port/"
            )
            default_ca_args=(--portable-runtime-probe --ca-only)

            check_probe_output() {
              grep -F 'portable-ca-bundle=ok' "$1"
              grep -F 'portable-fontconfig=ok' "$1"
              grep -F 'portable-pgrep=ok' "$1"
              grep -F 'portable-ca-configuration=ok' "$1"
              grep -F 'portable-qt-https=ok' "$1"
              grep -F 'portable-qt-wss=ok' "$1"
            }

            tolerate_launcher_restriction() {
              grep -Eq \
                'cannot (unshare|write (uid|gid)_map)|fuse: (device /dev/fuse not found|failed to open /dev/fuse)' \
                "$1"
            }

            if "$image" --version > version.txt 2>&1; then
              cat version.txt
              grep -F "Chatterino" version.txt
            else
              # The direct launcher needs FUSE, while extract-and-run needs an
              # unprivileged user namespace. Nix and GitHub build sandboxes can
              # prohibit either mechanism, so tolerate only those precise
              # launcher failures and exercise the payload through proot below.
              if ! tolerate_launcher_restriction version.txt; then
                cat version.txt
                exit 1
              fi
            fi

            if env CHATTERINO_SSL_CERT_FILE="$TMPDIR/ca.crt" \
              "$image" "''${probe_args[@]}" > direct-probe.txt 2>&1
            then
              check_probe_output direct-probe.txt
            elif ! tolerate_launcher_restriction direct-probe.txt; then
              cat direct-probe.txt
              exit 1
            fi

            if env -u CHATTERINO_SSL_CERT_FILE \
              "$image" "''${default_ca_args[@]}" > direct-default-ca.txt 2>&1
            then
              grep -F 'portable-ca-configuration=ok' direct-default-ca.txt
            elif ! tolerate_launcher_restriction direct-default-ca.txt; then
              cat direct-default-ca.txt
              exit 1
            fi

            if env APPIMAGE_EXTRACT_AND_RUN=1 \
              CHATTERINO_SSL_CERT_FILE="$TMPDIR/ca.crt" \
              "$image" "''${probe_args[@]}" > extract-run-probe.txt 2>&1
            then
              check_probe_output extract-run-probe.txt
            elif ! tolerate_launcher_restriction extract-run-probe.txt; then
              cat extract-run-probe.txt
              exit 1
            fi

            if env -u CHATTERINO_SSL_CERT_FILE APPIMAGE_EXTRACT_AND_RUN=1 \
              "$image" "''${default_ca_args[@]}" > extract-default-ca.txt 2>&1
            then
              grep -F 'portable-ca-configuration=ok' extract-default-ca.txt
            elif ! tolerate_launcher_restriction extract-default-ca.txt; then
              cat extract-default-ca.txt
              exit 1
            fi

            # Inspect the same squashfs payload exercised above without
            # unpacking a third full copy into the constrained CI sandbox.
            offset="$("$image" --appimage-offset)"
            unsquashfs -o "$offset" -ll "$image" > payload-files.txt
            grep -F '${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt' payload-files.txt
            grep -F '${pkgs.fontconfig.out}/etc/fonts/fonts.conf' payload-files.txt
            grep -F '${pkgs.procps}/bin/pgrep' payload-files.txt
            grep -F '${runtimeProbe}/bin/chatterino-portable-runtime-probe' payload-files.txt
            grep -F '${pkgs.openssl.out}/etc/ssl/openssl.cnf' payload-files.txt
            grep -F '${pkgs.openssl.out}/lib/ossl-modules/legacy.so' payload-files.txt
            grep -F '/com.chatterino.chatterino.desktop' payload-files.txt
            grep -F '/com.chatterino.chatterino.png' payload-files.txt

            # Hosted CI can prohibit the user namespace used by both AppImage
            # launch forms. Selectively extract only the runtime probe closure
            # and execute those exact payload files through proot. This keeps
            # the fallback functional without unpacking the full 838 MiB image.
            mapfile -t probe_paths < <(
              sed 's#^/##' ${runtimeProbeClosure}/store-paths
            )
            unsquashfs -o "$offset" -d probe-root "$image" \
              "''${probe_paths[@]}" >/dev/null
            test -s "probe-root${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
            test -s "probe-root${pkgs.fontconfig.out}/etc/fonts/fonts.conf"
            test -x "probe-root${pkgs.procps}/bin/pgrep"
            test -x "probe-root${runtimeProbe}/bin/chatterino-portable-runtime-probe"

            FONTCONFIG_FILE=${pkgs.fontconfig.out}/etc/fonts/fonts.conf \
              FONTCONFIG_PATH=${pkgs.fontconfig.out}/etc/fonts \
              proot -R probe-root ${pkgs.fontconfig.bin}/bin/fc-match \
              sans-serif >/dev/null
            proot -R probe-root ${pkgs.procps}/bin/pgrep --version >/dev/null
            CHATTERINO_SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt \
              proot -R probe-root \
              ${runtimeProbe}/bin/chatterino-portable-runtime-probe \
              --ca-only 2>&1 | tee payload-default-ca.txt
            grep -F 'portable-ca-configuration=ok' payload-default-ca.txt
            {
              echo 'portable-ca-bundle=ok'
              echo 'portable-fontconfig=ok'
              echo 'portable-pgrep=ok'
              CHATTERINO_SSL_CERT_FILE=/runtime-test-ca.pem \
                proot -R probe-root \
                -b "$TMPDIR/ca.crt:/runtime-test-ca.pem" \
                ${runtimeProbe}/bin/chatterino-portable-runtime-probe \
                "''${probe_args[@]:1}"
            } 2>&1 | tee payload-probe.txt
            check_probe_output payload-probe.txt
            touch "$out"
          '';
    in
    {
      overlays.default = chatterinoOverlay;

      packages = forAllSystems (
        system:
        let
          validation = validationFor system;
          pkgs = import nixpkgs { inherit system; };
        in
        {
          rumble-validate-public = validation.public;
          rumble-diagnose-channel = validation.diagnose;
          rumble-validate-authenticated = validation.authenticated;
          rumble-validation-self-test = validation.selfTest;
          rumble-streamlink-plugin = validation.streamlinkPlugin;
          rumble-streamlink-self-test = validation.streamlinkSelfTest;
          rumble-validation = validation.aggregate;
          rumble-browser-support = pkgs.selenium-manager;
          default = validation.aggregate;
        }
        // nixpkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          chatterino-ui-controller = uiControllerFor system;
          chatterino-ui-xephyr = uiHarnessFor system;
          rumble-credential-store-probe = credentialStoreProbeFor system;
        }
        // nixpkgs.lib.optionalAttrs (system == "x86_64-linux") {
          chatterino-windows-cross = (portableWindowsFor system).chatterino;
          chatterino-windows-portable = (portableWindowsFor system).portable;
          chatterino-linux-appimage = portableLinuxFor system;
        }
      );

      apps = forAllSystems (
        system:
        let
          validation = validationFor system;
          pkgs = import nixpkgs { inherit system; };
        in
        {
          rumble-validate-public = {
            type = "app";
            program = "${validation.public}/bin/rumble-validate-public";
          };
          rumble-diagnose-channel = {
            type = "app";
            program = "${validation.diagnose}/bin/rumble-diagnose-channel";
          };
          rumble-validate-authenticated = {
            type = "app";
            program = "${validation.authenticated}/bin/rumble-validate-authenticated";
          };
          rumble-validation-self-test = {
            type = "app";
            program = "${validation.selfTest}/bin/rumble-validation-self-test";
          };
          rumble-streamlink-self-test = {
            type = "app";
            program = "${validation.streamlinkSelfTest}/bin/rumble-streamlink-self-test";
          };
        }
        // nixpkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          chatterino-ui-controller = {
            type = "app";
            program = "${uiControllerFor system}/bin/chatterino-ui-xephyr";
          };
          chatterino-ui-xephyr = {
            type = "app";
            program = "${uiHarnessFor system}/bin/chatterino-ui-xephyr";
          };
        }
      );

      checks = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          rumble-validation = (validationFor system).check;
        }
        // nixpkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          rumble-credential-storage = credentialStoreTestFor system;
        }
        // nixpkgs.lib.optionalAttrs (system == "x86_64-linux") {
          chatterino-windows-portable = (portableWindowsFor system).portable;
          chatterino-linux-appimage = portableLinuxRuntimeCheckFor system;
          chatterino-nix-package =
            (import nixpkgs {
              inherit system;
              overlays = [ chatterinoOverlay ];
            }).chatterino7;
        }
      );

      homeModules.default = import ./modules/home-manager.nix { inherit self; };
      homeModules.chatterino-tabemit = self.homeModules.default;
    };
}
