# Continuous integration

Every pull request first runs a cheap offline preflight covering Python syntax,
CI/integration policy, validators, mocked Streamlink fixtures, and the
user-facing string audit. Only after that succeeds does it start the C++ suite
and five locked-Nix contract checks from the same workflow revision. The
locked Rumble-validation build also runs Streamlink discovery against the
packaged Streamlink dependency. The Nix check name includes its exact flake
attribute: `chatterino-nix-package`, `chatterino-windows-portable`,
`chatterino-linux-appimage`, `rumble-validation`, or
`rumble-credential-storage`. The `Required PR gate` fails unless preflight,
every matrix entry, and the C++ suite succeeded. `Build Nix package` is a compatibility
context retained during branch-protection migration; do not remove it until
the protected ruleset requires `Required PR gate` instead. Superseded runs are
cancelled by the workflow concurrency key.

Pull-request runs also resolve the live head, live base-ref target, unresolved
review threads, review decision, matching ruleset details, and required-check
app identities. The aggregate requires that integration decision on pull
requests; push and manual runs require the PR-only job to be skipped.

The ordinary Nix matrix uses only the repository flake and lock file. It
evaluates the package and portable contracts and builds the Rumble validation
and credential-storage tests in independent matrix entries. Full Nix
validation builds the repository-defined Chatterino package on `main`, the
portable-artifact opt-in, or manual dispatch. Neither path imports a mutable
nixpkgs revision or uses `--impure`.

The complete CTest suite runs once. Its first failure produces the JUnit file,
CTest directory, and service logs as artifacts. Repeated focused matrices are
diagnostic stress tests with `--gtest_break_on_failure`; they cannot turn a
full-suite failure into success. The workflow invokes
`tools/ci/run-required-ctest`, and its coupled synthetic fail-then-pass test
asserts exactly one invocation and preserves the original nonzero status.

When a suite fails, treat that first exact-head failure as the result: download
`chatterino-test-diagnostics`, inspect `test-results.xml` and `/tmp/ctest.log`,
then reproduce the failing test locally or in the pinned container. Do not
re-run the required suite to obtain a green result. A focused repeat may be
used to collect extra evidence, but it must stop at its first observed failure.

Portable Windows and AppImage artifacts are intentionally demand-driven:
they run on `main`, when a pull request receives the `portable-artifacts`
label, or when a manual dispatch selects **Build portable application
artifacts**. Applying or removing that label triggers a new workflow run. When
requested, both full Nix and artifact jobs become part of the aggregate gate.
Those artifacts are built only from the locked flake and are checksum-verified
before upload; the AppImage runtime check and the actual artifact output are
built separately.

The immutable CI inputs are:

| Component | Version | Immutable identity | Update path |
| --- | --- | --- | --- |
| Checkout action | v7.0.1 | commit `3d3c42e5aac5ba805825da76410c181273ba90b1` | weekly Dependabot |
| Artifact action | v7.0.1 | commit `043fb46d1a93c77aae656e7c1c64a875d1fc6a0a` | weekly Dependabot |
| Install-Nix action | v31.11.0 | commit `630ae543ea3a38a9a4166f03376c02c50f408342` | weekly Dependabot |
| C++ test image | Ubuntu 26.04 | OCI digest in `ci.yml` | reviewed manual digest bump |
| HTTPBox | v0.2.1 | release-archive SHA-256 in `ci.yml` | reviewed tag/hash bump |
| Twitch PubSub test server | v1.0.12 | release-archive SHA-256 in `ci.yml` | reviewed tag/hash bump |
| Nix inputs | repository lock | revisions and nar hashes in `flake.lock` | reviewed lock update |

The test container is selected by digest. Downloaded helper archives are
checked before extraction, and the job records the container identity plus
helper tags and hashes. GitHub Actions use full commit SHAs with readable
version comments; checkout and artifact upload use their Node-24 releases.
The offline CI policy tests reject floating actions, mutable containers,
missing or late helper checksums, impure Nix evaluation, serial required
lanes, and incomplete aggregation before dependencies are installed.

When updating a pin, verify the upstream tag-to-commit association, review the
complete action diff, and preserve the readable version comment. For a helper,
download the named release asset, calculate its SHA-256 independently, update
the tag and hash together, and confirm the negative checksum fixture still
fails before extraction. For the container, resolve the intended platform
manifest to its digest. For Nix inputs, review the lock-file diff and evaluate
all five attributes. Every pin bump requires fresh exact-head CI.

The remaining mutable trust boundaries are the GitHub-hosted
`ubuntu-latest` runner image, the package indexes used inside the pinned build
container, and upstream availability of already-pinned content. They do not
change the selected action commits, container filesystem, helper bytes, or
locked Nix dependency graph, but an outage can still fail a run.

## Clean reproduction

From a clean checkout of the reviewed commit, the ordinary locked-Nix gate can
be reproduced without writing the lock file:

```sh
nix flake metadata --no-write-lock-file
nix eval --raw .#checks.x86_64-linux.chatterino-nix-package.name
nix eval --raw .#checks.x86_64-linux.chatterino-windows-portable.name
nix eval --raw .#checks.x86_64-linux.chatterino-linux-appimage.name
```

For deterministic test evidence, build
`.#checks.x86_64-linux.rumble-validation` and
`.#checks.x86_64-linux.rumble-credential-storage`. For the full,
demand-driven Nix validation used by release and packaging work, build
`.#checks.x86_64-linux.chatterino-nix-package`. Both paths use the same lock
file and never enable `--impure`.
