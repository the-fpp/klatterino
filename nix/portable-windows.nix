{
  pkgs,
  chatterino7Src,
  revision,
  nixpkgsRevision,
}:

let
  lib = pkgs.lib;
  windowsOverlay =
    final: prev:
    let
      # Keep all target-specific fixes inside the MinGW dependency graph.  A
      # top-level override would also invalidate Qt's native host tools through
      # package splicing and rebuild hundreds of unrelated Linux packages.
      # The MinGW giflib patch in this pinned nixpkgs revision no longer
      # applies to giflib 6.1.3.  Qt only needs libwebp itself for its image
      # plugin, not libwebp's GIF/TIFF conversion tools, so do not pull those
      # unrelated command-line dependencies into the target graph.
      libwebpWindows =
        (prev.libwebp.override {
          gifSupport = false;
          jpegSupport = false;
          pngSupport = false;
          tiffSupport = false;
        }).overrideAttrs
          (old: {
            # The MinGW shared build does not export the mux entry points consumed
            # by QtImageFormats.  Link this small codec into the Qt plugin instead;
            # this also removes four otherwise private codec DLLs from the archive.
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              "-DBUILD_SHARED_LIBS=OFF"
            ];
          });
      sqliteWindows = prev.sqlite.overrideAttrs (old: {
        nativeBuildInputs = builtins.filter (input: (input.pname or "") != "tcl") (
          old.nativeBuildInputs or [ ]
        );
        configureFlags = map (flag: if lib.hasPrefix "--with-tcl=" flag then "--disable-tcl" else flag) (
          old.configureFlags or [ ]
        );
        doCheck = false;
      });
      harfbuzzWindows =
        (prev.harfbuzz.override {
          withGraphite2 = false;
          withIntrospection = false;
        }).overrideAttrs
          (old: {
            nativeBuildInputs = builtins.filter (input: (input.pname or "") != "glib") (
              old.nativeBuildInputs or [ ]
            );
            buildInputs = builtins.filter (input: (input.pname or "") != "glib") (old.buildInputs or [ ]);
            mesonFlags = (old.mesonFlags or [ ]) ++ [
              "-Dglib=disabled"
              "-Dgobject=disabled"
              "-Dtests=disabled"
              "-Ddocs=disabled"
            ];
            postInstall = (old.postInstall or "") + ''
              # The upstream expression declares a devdoc output even when the
              # cross build disables gtk-doc.  Materialize the intentionally empty
              # output so Nix does not reject the otherwise successful build.
              mkdir -p "$devdoc"
            '';
            doCheck = false;
          });
      replaceQtbaseDependencies = map (
        input:
        if (input.pname or "") == "sqlite" then
          sqliteWindows
        else if (input.pname or "") == "harfbuzz" then
          harfbuzzWindows
        else
          input
      );
      filterQtbaseDependencies = builtins.filter (
        input:
        !builtins.elem (input.pname or "") [
          # Windows Qt links the platform OpenGL libraries directly.  The
          # generic nixpkgs input would otherwise cross-build GLVND and X11.
          "libglvnd"
        ]
      );
      qtbaseWindows = prev.kdePackages.qtbase.overrideAttrs (old: {
        buildInputs = replaceQtbaseDependencies (filterQtbaseDependencies (old.buildInputs or [ ]));
        propagatedBuildInputs = replaceQtbaseDependencies (
          filterQtbaseDependencies (old.propagatedBuildInputs or [ ])
        );
      });
      qtsvgWindows =
        (prev.kdePackages.qtsvg.override {
          qtbase = qtbaseWindows;
        }).overrideAttrs
          (old: {
            buildInputs = map (input: if (input.pname or "") == "libwebp" then libwebpWindows else input) (
              builtins.filter (
                input:
                !builtins.elem (input.pname or "") [
                  "jasper"
                  "libmng"
                ]
              ) (old.buildInputs or [ ])
            );
          });
      qtkeychainWindows =
        (prev.kdePackages.qtkeychain.override {
          qtbase = qtbaseWindows;
        }).overrideAttrs
          (old: {
            buildInputs = builtins.filter (
              input:
              !builtins.elem (input.pname or "") [
                "libsecret"
                "qttools"
              ]
            ) (old.buildInputs or [ ]);
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [
              # QtKeychain only needs QtTools to compile its seven translation
              # catalogs.  Cross-building that complete target tool suite is
              # unnecessary for the native Credential Manager backend.
              "-DBUILD_TRANSLATIONS=OFF"
            ];
            meta = (old.meta or { }) // {
              platforms = [ final.stdenv.hostPlatform.system ];
              broken = false;
            };
          });
      qtimageformatsWindows =
        (prev.kdePackages.qtimageformats.override {
          qtbase = qtbaseWindows;
        }).overrideAttrs
          (old: {
            buildInputs = map (input: if (input.pname or "") == "libwebp" then libwebpWindows else input) (
              builtins.filter (
                input:
                !builtins.elem (input.pname or "") [
                  "jasper"
                  "libmng"
                  "libtiff"
                ]
              ) (old.buildInputs or [ ])
            );
          });
      kdeWindows = prev.kdePackages // {
        qtbase = qtbaseWindows;
        qtsvg = qtsvgWindows;
        qtkeychain = qtkeychainWindows;
        qtimageformats = qtimageformatsWindows;
      };
      windeployqtStub = final.buildPackages.writeShellScript "windeployqt-cross-stub" ''
        # Runtime DLLs and Qt plugins are assembled and audited by the
        # portable archive derivation; a Windows windeployqt binary cannot
        # execute on the Linux build host.
        exit 0
      '';
      chatterinoWindows = prev.chatterino2.buildChatterino {
        enableAvifSupport = false;
        kdePackages = kdeWindows;
      };
    in
    {
      chatterino7 = chatterinoWindows.overrideAttrs (old: {
        pname = "chatterino7";
        version = prev.chatterino7.version;
        src = chatterino7Src;
        patches = [
          ./patches/chatterino-cross-windows-install.patch
        ];
        # wrapQtAppsHook produces Unix launch scripts.  The portable archive
        # instead places the target Qt DLL and plugin closure beside the PE.
        dontWrapQtApps = true;
        buildInputs = builtins.filter (
          input:
          !builtins.elem (input.pname or "") [
            "libsecret"
            "libavif"
            "qt5compat"
          ]
        ) (old.buildInputs or [ ]);
        nativeBuildInputs = builtins.filter (input: !lib.hasPrefix "wrap-qt" (input.pname or "")) (
          old.nativeBuildInputs or [ ]
        );
        cmakeFlags = (old.cmakeFlags or [ ]) ++ [
          "-DCHATTERINO_NO_AVIF_PLUGIN=ON"
          "-DWINDEPLOYQT_PATH=${windeployqtStub}"
        ];
        meta = (prev.chatterino7.meta or { }) // {
          platforms = [ final.stdenv.hostPlatform.system ];
          broken = false;
        };
      });
      chatterino7PortableRuntime = {
        inherit
          qtbaseWindows
          qtsvgWindows
          qtimageformatsWindows
          qtkeychainWindows
          ;
        libjpeg = prev.libjpeg_turbo;
        seleniumManager = prev.selenium-manager;
      };
    };

  cross = pkgs.pkgsCross.mingwW64.extend windowsOverlay;
  chatterino = cross.chatterino7;
  runtime = cross.chatterino7PortableRuntime;
  shortRevision =
    builtins.substring 0 12 revision + lib.optionalString (lib.hasSuffix "-dirty" revision) "-dirty";
  archiveName = "chatterino-${shortRevision}-windows-x86_64.zip";
  manifest = pkgs.writeText "chatterino-windows-manifest.json" (
    builtins.toJSON {
      artifact = archiveName;
      format = "portable ZIP";
      inherit revision;
      system = "x86_64-windows";
      buildSystem = pkgs.stdenv.hostPlatform.system;
      nixpkgs = nixpkgsRevision;
      toolchain = "Nix pkgsCross.mingwW64 (MinGW-w64/GCC)";
      credentialStore = "QtKeychain Windows Credential Manager backend";
      seleniumManager = runtime.seleniumManager.version;
      loginBrowserPolicy = "installed system browser; no bundled browser engine";
      limitations = [
        "Native Windows toast notifications are disabled in the MinGW build because the vendored WinToast implementation requires Microsoft WRL."
        "QtKeychain translation catalogs are omitted; credential storage is fully included."
      ];
    }
  );
  portable =
    pkgs.runCommand "chatterino-windows-portable-${shortRevision}"
      {
        nativeBuildInputs = [
          pkgs.coreutils
          pkgs.findutils
          pkgs.gnused
          pkgs.python3
          pkgs.zip
          cross.stdenv.cc.bintools.bintools
        ];
      }
      ''
        set -euo pipefail

        bundle="$TMPDIR/chatterino"
        mkdir -p \
          "$bundle/iconengines" \
          "$bundle/imageformats" \
          "$bundle/licenses" \
          "$bundle/networkinformation" \
          "$bundle/platforms" \
          "$bundle/styles" \
          "$bundle/tls" \
          "$out"

        cp -L ${chatterino}/bin/* "$bundle/"
        install -Dm755 \
          ${runtime.seleniumManager}/bin/selenium-manager.exe \
          "$bundle/selenium-manager.exe"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/platforms/qwindows.dll \
          "$bundle/platforms/qwindows.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/styles/qmodernwindowsstyle.dll \
          "$bundle/styles/qmodernwindowsstyle.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/networkinformation/qnetworklistmanager.dll \
          "$bundle/networkinformation/qnetworklistmanager.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/tls/qopensslbackend.dll \
          "$bundle/tls/qopensslbackend.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/tls/qschannelbackend.dll \
          "$bundle/tls/qschannelbackend.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/imageformats/qgif.dll \
          "$bundle/imageformats/qgif.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/imageformats/qico.dll \
          "$bundle/imageformats/qico.dll"
        install -Dm644 \
          ${runtime.qtbaseWindows}/lib/qt-6/plugins/imageformats/qjpeg.dll \
          "$bundle/imageformats/qjpeg.dll"
        install -Dm644 \
          ${runtime.qtsvgWindows}/lib/qt-6/plugins/imageformats/qsvg.dll \
          "$bundle/imageformats/qsvg.dll"
        install -Dm644 \
          ${runtime.qtsvgWindows}/lib/qt-6/plugins/iconengines/qsvgicon.dll \
          "$bundle/iconengines/qsvgicon.dll"
        install -Dm644 \
          ${runtime.qtimageformatsWindows}/lib/qt-6/plugins/imageformats/qwebp.dll \
          "$bundle/imageformats/qwebp.dll"
        install -Dm644 \
          ${runtime.libjpeg.bin}/bin/libjpeg-62.dll \
          "$bundle/libjpeg-62.dll"

        install -Dm644 ${chatterino7Src}/LICENSE \
          "$bundle/licenses/Chatterino.txt"
        install -Dm644 ${../docs/THIRD-PARTY-NOTICES.md} \
          "$bundle/licenses/THIRD-PARTY-NOTICES.md"
        install -Dm644 ${chatterino7Src}/lib/expected-lite/LICENSE.txt \
          "$bundle/licenses/expected-lite.txt"
        install -Dm644 ${chatterino7Src}/lib/libcommuni/LICENSE \
          "$bundle/licenses/LibCommuni.txt"
        install -Dm644 ${chatterino7Src}/lib/lrucache/LICENSE \
          "$bundle/licenses/LRUCache.txt"
        install -Dm644 ${chatterino7Src}/lib/magic_enum/LICENSE \
          "$bundle/licenses/magic-enum.txt"
        install -Dm644 ${chatterino7Src}/lib/miniaudio/LICENSE \
          "$bundle/licenses/miniaudio.txt"
        install -Dm644 ${chatterino7Src}/lib/rapidjson/license.txt \
          "$bundle/licenses/RapidJSON.txt"
        install -Dm644 ${chatterino7Src}/lib/serialize/LICENSE \
          "$bundle/licenses/pajlada-serialize.txt"
        install -Dm644 ${chatterino7Src}/lib/settings/LICENSE \
          "$bundle/licenses/pajlada-settings.txt"
        install -Dm644 ${chatterino7Src}/lib/signals/LICENSE \
          "$bundle/licenses/pajlada-signals.txt"
        install -Dm644 ${chatterino7Src}/lib/sol2/LICENSE.txt \
          "$bundle/licenses/sol2.txt"
        install -Dm644 ${runtime.seleniumManager.src}/LICENSE \
          "$bundle/licenses/Selenium.txt"
        install -Dm644 \
          ${chatterino7Src}/lib/twitch-eventsub-ws/lib/date/LICENSE.txt \
          "$bundle/licenses/date.txt"
        install -Dm644 \
          ${chatterino7Src}/lib/twitch-eventsub-ws/lib/fmt/LICENSE \
          "$bundle/licenses/fmt.txt"
        install -Dm644 ${manifest} "$bundle/manifest.json"

        # PE files can contain compiler assertion source paths. Replace the two
        # equal-length host prefixes without changing offsets or executable data.
        find "$bundle" -type f \( -iname '*.exe' -o -iname '*.dll' \) \
          -exec sed -i \
            -e 's#/nix/store/#C:/src/nix/#g' \
            -e 's#/build/#C:/src/#g' {} +

        if grep -R -a -l -E '/nix/store/|/build/' "$bundle"; then
          echo "portable artifact retains a host build path" >&2
          exit 1
        fi

        python3 ${./audit-pe-closure.py} \
          --objdump x86_64-w64-mingw32-objdump \
          "$bundle" > "$bundle/PE-IMPORTS.tsv"

        (
          cd "$TMPDIR"
          zip -X -9 -r "$out/${archiveName}" chatterino
        )
        install -Dm644 "$bundle/manifest.json" "$out/manifest.json"
        cp -R "$bundle/licenses" "$out/licenses"
        install -Dm644 "$bundle/PE-IMPORTS.tsv" "$out/PE-IMPORTS.tsv"
        (
          cd "$out"
          sha256sum \
            "${archiveName}" \
            manifest.json \
            licenses/* \
            PE-IMPORTS.tsv > SHA256SUMS
          sha256sum --check SHA256SUMS
        )
      '';
in
{
  inherit cross;
  inherit chatterino portable;
}
