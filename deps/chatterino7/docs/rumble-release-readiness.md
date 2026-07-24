# Rumble release readiness

> **IMPLEMENTATION-COMPLETE CANDIDATE — not release approval.** The
> deterministic gate incorporates the merged public resolver compatibility
> (#56), session/send (#20), moderation (#12), multi-channel (#14), and
> redacted runtime diagnostics/user guidance (#21) contracts. Only reviewed
> exact-head CI and the named external closure evidence remain.

This document is the release, capability, and rollback checklist for the
vendored Rumble provider. It records only behavior implemented on the reviewed
repository head. It does not promote observed provider behavior to a stable
Rumble API, perform live discovery, or make an operator report part of CI.

## Capability matrix

| Capability | Current state | Release evidence and boundary |
|---|---|---|
| Public locator validation and canonical layout identity | Implemented | Public channel, video, and embed locators are canonicalized before persistence. Unsafe input becomes a visible typed placeholder and starts no request. |
| Public live/offline resolution | Implemented with bounded compatibility repair by merged #56 | The page scanner ignores unrelated hydration, layout, and inline-SVG tags within the existing 1 MiB page bound, while title, interstitial, inert/raw-text, iframe, typed-script, ambiguity, unique-attribute, and 4 KiB contract-tag checks remain strict. The Python validator and C++ resolver share this accepted shape. An access/challenge page is a transport rejection with “The service rejected the request.”; malformed contract-bearing input is a protocol failure with “The response contract was not recognized.” Live, offline, HTTP, media, limit, timeout, cancellation, and transport outcomes remain typed. |
| Credential-free page/embed diagnostic | Implemented by merged #57 and reconciled by #56 | `rumble-diagnose-channel` follows the bounded public resolver stages and emits only schema-versioned, closed-vocabulary state. It isolated the prior `page_schema` mismatch without printing the locator, response, IDs, headers, cookies, or exception text; its accepted page contract now matches production. It is a support tool, not a substitute for deterministic fixtures. |
| Anonymous bootstrap and realtime | Implemented | One bounded SSE reader requires `init`, preserves typed events, deduplicates IDs across reconnect overlap, and publishes deterministic message/state transitions. |
| Failure lifecycle and live-to-ended propagation | Implemented; actual-application public transition observed 2026-07-21 | Retryable failures use bounded jitter/backoff; valid `Retry-After` is honored; cancellation, owner destruction, and application shutdown retire requests and timers. Clean EOF/404/410 revalidate the persistent public locator without a false offline flash. Only authoritative offline evidence clears live/writable/completion state and emits the selected Rumble source as unavailable to the idempotent follower stop path. A previously connected public selection changed to offline without reopening the tab and remained offline on an immediate repeated status read; no channel, account, message, URL, screenshot, or raw provider data was retained. No send is retried here. |
| Message identity, text, links, timestamps, order, delete, and pin state | Implemented | Typed provider events feed production `RumbleChannel`/message seams. Rumble author elements keep provider-native colors and links while explicitly excluding Twitch/7TV username paints, including for the current account's echoed sends. A confirmed send's provider ID binds one optimistic row; the authoritative realtime event or reconnect bootstrap atomically replaces it, and event-before-response delivery does not create a second copy. Raw payloads stay inside the provider. |
| Badge and role presentation metadata | Implemented | Accepted Rumble-hosted badge icons render through Chatterino's badge elements with escaped labels and normal visibility categories. Iconless badge IDs and role IDs remain metadata only. Role metadata does not itself grant a current-account or moderation capability. |
| Rumble-native emote catalog, image rendering, completion, and send encoding | Implemented by #101 | A bounded public per-stream catalog authorizes exact global `:r+name:` and channel `:name:` occurrences. Accepted HTTPS assets render through shared image infrastructure and hydrated messages retain immutable snapshots. Validated accounts receive only eligibility-proven candidates; insertion uses Rumble's accepted ordinary shortcode text and existing exactly-once routing. Unknown colon text remains text. |
| Accounts, browser sign-in, secure session storage, and normal text send | Implemented; native browser acquisition still requires attended release smoke | The shared Accounts UI supports multiple Rumble accounts and selection from the person menu. A short-lived installed Firefox/Chromium-family browser and matching Selenium-managed driver acquire the session, then both exit. Validated credentials use QtKeychain with no plaintext fallback; only non-secret metadata enters settings. One at-most-once normal-text send is enabled. Authentication loss, destination denial, rate limiting, ambiguous delivery, cancellation, and stale generations remain distinct. Structured reply metadata remains unsupported; the complete visible reply text uses the same ordinary one-shot path. |
| User identity, current-account roles, and moderation actions | Inbound state implemented; outbound actions explicitly unsupported by merged #12 | Provider identities remain opaque and scoped. Accepted delete, pin, and typed mute/role state reduces idempotently with bounded memory. An authenticated or moderator-labelled account never implies an outbound endpoint: delete, pin, mute, and ban mutations all return the explicit unsupported result and dispatch nothing. |
| Rumble-only and mixed multi-channel selection, persistence, completion, routing, and tab-emit | Implemented by merged #14 plus #118/#124 behavior | Canonical Rumble locators and selected rows round-trip in one-Rumble, alias/multiple-Rumble, and mixed Twitch/Kick/Rumble layouts. Completion and submission read current capability snapshots; an accepted normal-text draft reaches exactly one destination. A Rumble reply retires its unsupported relation and sends the visible text through that one-shot path; recoverable failures retain the ordinary draft. Unsupported commands and ambiguous sends preserve their drafts. Tab-emit exposes only the revalidated canonical locator, never the runtime identity, and includes the selected Rumble child's provider-confirmed `is_live` value so an ended source can stop follower playback without resolver noise. |
| Stable channel labels and live stream titles | Implemented | Rumble tab labels use stable channel identity. The selected live child supplies its current stream title only to the split header; title refresh, confirmed-offline clearing, reconnection, independent tabs, and mixed-tab selection are covered without renaming the aggregate tab. |
| Redacted status command, structured diagnostics, and final user guide | Implemented by merged #21 | `/rumble-status` formats an immutable, identity-free owner-thread snapshot without requesting, reconnecting, validating, or mutating timers. It distinguishes lifecycle, bounded retry/rate-limit wait, session validation, destination denial, and closed-vocabulary error category/time. The dedicated `chatterino.rumble` logger uses the same safe vocabulary with bounded key memory, suppression, and emission rate; the [user guide](rumble-user-guide.md) covers clean-profile setup, imported-session handling, state actions, unsupported capabilities, and paste-safe reporting. |

No implementation dependency remains. The schema-1 public validator (#36),
#14's native layout/tab-emit report, and the native release smoke are external
closure evidence; they do not replace deterministic tests or block the reviewed
implementation PR. The accepted #40 conclusion has already been consumed and
must not be rerun for this gate.

## Deterministic release gate

Configure and build exactly like the repository test job, then run the named
suite repeatedly before the full test set:

```sh
cmake \
  -S deps/chatterino7 \
  -B build-test \
  -DBUILD_TESTS=ON \
  -DBUILD_BENCHMARKS=OFF \
  -DBUILD_APP=OFF \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR/lib/cmake" \
  -DCHATTERINO_STATIC_QT_BUILD=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test --parallel "$(nproc)"
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='RumbleReleaseGate.*:RumbleAccount.*:RumbleAuth*.*:RumbleSend.*:RumbleChannelCapability.*:RumbleChannelModeration.*:RumbleSecretRedaction.*:RumbleModeration*.*:RumbleMultiChannelIntegration.*:RumbleDiagnostic*.*:RumbleRedaction.*:RumbleStatusCommand.*:RumbleLogCoalescing.*:RumbleResolverParser.ToleratesBoundedUnrelatedHydrationMarkup:RumbleLifecycle.AccessInterstitialAndSchemaUseDistinctOperatorText-RumbleRedaction.ActualTransitionLogUsesOnlyClosedVocabulary' \
  --gtest_repeat=10 \
  --gtest_break_on_failure
QT_QPA_PLATFORM=minimal ctest \
  --test-dir build-test \
  --output-on-failure
```

`RumbleReleaseGate.*` is intentionally a thin cross-layer gate. It constructs
the production application controller, provider/channel, connection, typed
API, session, moderation reducer, aggregate routing, and message path over
scripted HTTP/SSE/auth transports. It covers public bootstrap-to-realtime
reconnect with duplicate/malformed input, offline and unsafe locators, rate
limiting, in-memory active authenticated text, explicit unsupported moderation,
canonical one/multiple/mixed child rows, exactly-one routing, and aggregate
shutdown residue. The combined repeat filter keeps the detailed account,
moderation, multi-channel, immutable status-command, redaction, and bounded-log
permutations authoritative without copying them into this thin gate. It also
runs #56's two narrow cross-layer release regressions: tolerated unrelated page
markup and distinct access-versus-schema operator behavior.

`RumbleRedaction.ActualTransitionLogUsesOnlyClosedVocabulary` is excluded only
from the in-process repeat command because the production process-global
60-second status-log coalescer correctly suppresses the identical transition on
later iterations. The test remains authoritative in the fresh-process full
CTest invocation above. `RumbleRedaction.RejectsHostileValuesInsteadOfReflectingThem`
and the local `RumbleLogCoalescing.*` state-machine tests remain in every
repeated iteration.

Run the repository validation and packaging gates too:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest -v \
  tools.tests.test_rumble_validation
nix build --no-write-lock-file --print-build-logs .#rumble-validation
nix flake check --no-write-lock-file --print-build-logs
```

The exact final pull-request head must pass both GitHub Actions jobs, **Build
and run Chatterino tests** and **Build Nix package**. A green run for an older
head is not reusable.

## Build and clean-profile smoke

Check out the exact candidate SHA with submodules. Build Chatterino through the
same overlay expression as the package job; the repository's default flake
package is the validator bundle, not Chatterino:

```sh
nix build \
  --impure \
  --show-trace \
  --print-build-logs \
  --expr '
    let
      plugin = builtins.getFlake (toString ./.);
      nixpkgs = builtins.getFlake "github:NixOS/nixpkgs/nixos-unstable";
      pkgs = import nixpkgs.outPath {
        system = builtins.currentSystem;
        overlays = [ plugin.overlays.default ];
      };
    in
      pkgs.chatterino7
  '
```

On Linux, isolate the native smoke from the normal profile before launching
`result/bin/chatterino`:

```sh
profile="$(mktemp -d)"
mkdir -p "$profile"/{home,config,data,cache,state}
export HOME="$profile/home"
export XDG_CONFIG_HOME="$profile/config"
export XDG_DATA_HOME="$profile/data"
export XDG_CACHE_HOME="$profile/cache"
export XDG_STATE_HOME="$profile/state"
result/bin/chatterino
```

Use only approved public test locators and a sanitized layout. Before the final
release decision, exercise one Rumble split and one mixed split, save/restart,
confirm the selected child/tab-emit state, repeat open/close/reconnect, and run
the rollback below. Run `chatterino-tabemit-listener --stdin` in a second
terminal when checking the selected-tab row. Do not paste channel/account
names, messages, IDs, URLs, listener payloads, screenshots, logs, or
credentials into the issue.

Within a Rumble split, `/rumble-status` is a local-only inspection command. Its
entire output is safe to paste into a public bug report; do not add raw logs,
locators, payloads, account details, or exception text. Follow the
[clean-profile setup and state actions](rumble-user-guide.md) rather than
attempting protocol discovery.

If a public channel is rejected, run the credential-free bounded diagnostic;
it sends no session/cookie material and prints only a paste-safe schema report:

```sh
nix run .#rumble-diagnose-channel -- --channel <public-channel-slug>
```

Paste the complete report beginning with the
`chatterino-rumble-diagnostic` marker. Never supplement it with a raw page,
HAR, cookie, unrestricted log, response excerpt, or account information.

Paste only this schema-1 result for the exact merged release SHA:

```text
<!-- rumble-release-native-validation schema=1 -->
RUMBLE_RELEASE_NATIVE_V1=PASS
tested_commit=<full merged SHA>
platform=<os-and-version>
clean_profile_install=PASS
rumble_layout_restore=PASS
mixed_layout_restore=PASS
tab_emit_selection=PASS
open_close_reconnect_cycles=PASS
disable_rollback=PASS
notes=none
```

Use `FAIL` and a short non-sensitive note when a row fails.

## Known limitations and privacy

- The anonymous reader depends on public page/embed/SSE shapes that Rumble can
  change. Typed schema/media/limit failures stop or retry safely; they are not
  silently reinterpreted.
- Anonymous history is the bounded `init` window only; pagination is not
  implemented. No post-head heartbeat is invented. HTTP and SSE-head work use
  a finite 20-second deadline.
- Automatic read recovery stops after three consecutive failures, uses full
  jitter capped at 30 seconds, and re-resolves an offline channel no faster
  than every 30 seconds. Ambiguous mutation delivery is never automatically
  retried.
- Validated graphical Rumble badges and catalog-backed native emotes are
  supported. Native emotes use the accepted ordinary text send; they do not add
  a separate mutation. Structured native replies remain unsupported, so their
  complete visible composer text degrades to one ordinary send. Every outbound
  moderation action is explicitly unsupported.
- The public HTML shape is provider-controlled. Unrelated bounded hydration,
  layout, and inline-SVG markup is ignored, but ambiguity, contract-bearing tag
  structure, origin, media, size, redirect, and duplicate-key checks remain
  fail-closed. An access/challenge response is never reported as schema drift.
- No code should log or persist raw runtime channel identities, query values,
  response bodies, cookies, sessions, user/message/request IDs, or exception
  text. Layout and tab-emit use revalidated public locators only.
- Live operator validation is never run in CI. Rumble account/session
  material is password-equivalent and must never enter this repository, a
  layout, a command line, an environment variable, or an issue report.
- `--safe-mode` disables Lua plugins; it does **not** disable the built-in
  Rumble provider and is not a protocol rollback.

## Disable and rollback

For immediate containment, close/remove Rumble splits from the sanitized
profile and restart. For a complete protocol rollback, stop Chatterino, pin the
flake input or deployment to a reviewed known-good commit/build from before
the Rumble provider change, rebuild/apply that deployment, and restart. Keep
the failed candidate profile isolated until triage is complete.

For a Home Manager input, the pin is explicit and reversible:

```nix
inputs.chatterinoPlugins.url =
  "github:the-fpp/klatterino/<KNOWN_GOOD_COMMIT>";
```

Update the lock for that input and run the normal Home Manager switch. To
restore the candidate, put back its exact reviewed SHA and switch again. Never
delete or rewrite the user's normal profile as a rollback step.

Release approval requires the reviewed deterministic matrix and both exact-head
CI jobs to be green, each named external report to satisfy its own schema, and
the native release report to match the merged release SHA.
