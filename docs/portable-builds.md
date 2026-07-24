# Portable Chatterino builds

The flake produces three Chatterino package forms from the same vendored source:

| Output | Intended host | Contents |
| --- | --- | --- |
| `chatterino7` through `overlays.default` | Nix | The regular wrapped Nix package |
| `chatterino-linux-appimage` | non-Nix x86_64 Linux | AppImage, manifest, license, and checksums |
| `chatterino-windows-portable` | x86_64 Windows | Portable ZIP, manifest, license, PE import report, and checksums |

Build both portable artifacts on x86_64 Linux:

```sh
nix build .#chatterino-windows-portable -o result-windows-portable
nix build .#chatterino-linux-appimage -o result-linux-appimage
```

Verify their external files:

```sh
(cd result-windows-portable && sha256sum --check SHA256SUMS)
(cd result-linux-appimage && sha256sum --check SHA256SUMS)
```

The Windows ZIP has one `chatterino` directory. Run `chatterino.exe` in place;
the sibling DLLs and Qt plugin directories are part of the application and must
remain beside it. The AppImage is directly executable on a distribution with a
working AppImage runtime. `APPIMAGE_EXTRACT_AND_RUN=1` is a useful fallback
when FUSE mounting is unavailable. The locked `nix-appimage` launcher also
requires unprivileged Linux user namespaces; this is independent of FUSE. On
NixOS, use either of those launch forms directly. Wrapping this artifact in
`appimage-run` is unnecessary and is not a supported launch path because it
nests two AppImage filesystem environments.

The AppImage uses a bundled Mozilla CA bundle and explicit fontconfig
configuration, so neither setting depends on the host's `/etc` links. It also
adds the bundled `pgrep` from procps to the application `PATH`. An administrator
who needs a private or enterprise CA can set `CHATTERINO_SSL_CERT_FILE` to a
readable PEM bundle. `CHATTERINO_FONTCONFIG_FILE` provides the equivalent
explicit override for unusual font setups.

Both builds retain the Rumble implementation and use QtKeychain for persistent
credentials. The Windows target includes QtKeychain's Windows Credential
Manager backend. The Linux AppImage includes the Linux secret-service backend.
Interactive Rumble login launches an installed system browser and Selenium
Manager/WebDriver as needed; neither artifact embeds a browser engine.
The Windows archive includes the locked native `selenium-manager.exe` beside
Chatterino, where the application discovers it without machine-wide setup. The
AppImage closure includes the corresponding locked Linux helper through the
normal Chatterino wrapper.

## Mechanical verification

`nix/audit-pe-closure.py` inspects the executable and every bundled Qt plugin
with the target `objdump`. The Windows build fails if two PE files have the same
basename, any binary is not native x86_64 Windows PE, or any non-system DLL
import is absent. This includes the bundled Selenium Manager helper, not only
Chatterino and Qt. It also fails if a copied PE retains a Nix store or
build-directory prefix. `PE-IMPORTS.tsv` records every import and whether it is
bundled or supplied by Windows. Qt 6 implements accessibility through its
Windows platform integration, so there is no separate accessibility plugin to
copy.

The AppImage builder computes its runtime closure from the wrapped Nix
application. Both output derivations generate revision-pinned JSON manifests
and verify their own `SHA256SUMS` before succeeding. CI builds and uploads the
exact artifact directories that passed on `main`, on pull requests labeled
`portable-artifacts`, or when manually requested; ordinary pull requests keep
portable packaging out of the required path. Its AppImage check
uses a local TLS server and a Qt probe from the exact image closure to exercise
HTTPS, WSS, fontconfig, and `pgrep` through both the normal launcher and
`APPIMAGE_EXTRACT_AND_RUN=1`. Build sandboxes can lack FUSE or prohibit the
launcher's user namespace, so only those specific launcher failures are
tolerated there. The check also audits the squashfs contents for the CA bundle,
fontconfig data,
`pgrep`, OpenSSL configuration and provider modules, runtime probe, desktop
file, and icon. It selectively extracts the probe's dependency closure and
repeats the Qt HTTPS/WSS, fontconfig, and `pgrep` checks through `proot`, so the
functional exact-payload fallback remains covered without unpacking the entire
image. `--version` is only an additional startup check.

For a public native diagnostic without opening the GUI, run the exact artifact
in both supported launch modes:

```sh
./chatterino-*-x86_64-linux.AppImage --portable-runtime-probe \
  --https-url https://rumble.com/ \
  --wss-url wss://eventsub.wss.twitch.tv/ws
APPIMAGE_EXTRACT_AND_RUN=1 \
  ./chatterino-*-x86_64-linux.AppImage --portable-runtime-probe \
  --https-url https://rumble.com/ \
  --wss-url wss://eventsub.wss.twitch.tv/ws
```

Native smoke validation should use a disposable profile so it cannot disturb
an existing Chatterino setup:

1. On x86_64 Windows, extract the ZIP, start `chatterino.exe`, add a Rumble
   channel, complete a Rumble login, restart Chatterino, and confirm the account
   remains available through Windows Credential Manager.
2. On a non-Nix x86_64 Linux installation, start the AppImage, add a Rumble
   channel, complete a Rumble login, restart it, and confirm the account remains
   available through the desktop secret service.
3. In both cases, confirm public chat loads, an authenticated message can be
   sent to an approved test channel, SVG/WebP badges render, and the login
   browser exits when the flow completes.

Wine is useful for diagnostics but is not native Windows acceptance evidence.
Live Rumble checks must follow the sanitized operator boundary documented in
the Rumble user guide; never attach credentials, raw traffic, or account data.

## Design record

The Windows build uses Nix's supported `pkgsCross.mingwW64` package set. It
keeps the normal nixpkgs Qt build expressions and applies narrowly scoped
target fixes for dependencies whose optional Unix tools do not cross-compile.
This was selected over:

- `pkgsStatic`, which targets static Unix binaries rather than Windows PE and
  does not provide a Windows Qt platform plugin;
- `nix bundle`, whose Linux launcher still depends on a Nix-oriented runtime
  model and does not produce a native Windows application;
- `nixcrpkgs`, which would add another cross-package ecosystem without removing
  the need to package and audit Qt's Windows plugin closure; and
- CMake `RUNTIME_DEPENDENCIES`/`windeployqt`, because their Windows scanners
  must execute on Windows and cannot run during a Linux cross build.

The Linux portable build uses the locked `nix-appimage` flake. It consumes the
same wrapped `chatterino7` package as regular Nix users, so its runtime closure
and browser/credential environment stay aligned with the supported Nix build.

The MinGW build has two explicit limitations recorded in its manifest:

- native Windows toast notifications are disabled because the vendored
  WinToast implementation requires Microsoft WRL, which MinGW does not ship;
- QtKeychain translation catalogs are omitted because target Qt Linguist tools
  cannot run on the Linux host. Credential Manager support remains enabled.
