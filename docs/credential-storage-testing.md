# Rumble credential storage testing

Rumble session credentials are stored through QtKeychain with plaintext
fallback disabled. On Linux, QtKeychain requires a Secret Service
implementation such as GNOME Keyring or KWallet.

The NixOS integration test uses two isolated virtual machines:

- one starts GNOME Keyring and verifies write, read from a new client process,
  delete, and missing-entry behavior;
- one intentionally has no Secret Service and verifies that Chatterino reports
  an actionable `Unavailable` result instead of a generic failure.

Neither VM connects to the host display, and the test uses only a compiled
synthetic canary credential.

Run the complete Linux integration test with:

```sh
nix build -L .#checks.x86_64-linux.rumble-credential-storage
```

The production-source probe can also be built independently:

```sh
nix build .#rumble-credential-store-probe
```

Its output contains operation status and backend diagnostics, but never prints
the stored credential.
