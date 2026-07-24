# Using Rumble channels

Rumble support reads public chat anonymously and can send ordinary text through
any selected Rumble account. The status command and logs described below are
local diagnostics: running them does not contact Rumble, reconnect, validate an
account, change a timer, or mutate channel state.

## Add a channel

1. Open Chatterino's channel picker for a split or multi-channel tab.
2. Select **Rumble**.
3. Enter a public channel slug such as `example`, or paste an accepted public
   Rumble channel, video, or embed URL.
4. Confirm the selection and wait while Chatterino resolves the public page.

A bare slug is accepted for convenience. Saved layouts retain a normalized
public HTTPS locator; private creator URLs, query strings, fragments, direct
chat-stream IDs, and foreign hosts are rejected. A failed saved locator restores
as a visible placeholder instead of silently contacting an unsafe endpoint.

Each Rumble split has its own public locator. Equivalent locators can share one
provider connection, while different locators keep independent lifecycle,
history, retry, and draft state. Multi-channel tabs route completion, history,
and ordinary sends to the active child and preserve every child's draft.
Rumble does not expose structured reply metadata. The reply picker remains
available: submitting a Rumble reply removes only that unsupported relationship
and sends the complete visible `@name` text once as an ordinary message. A
successful send clears the composer; login, offline, rate-limit, and transport
failures retain the visible text as an ordinary draft for retry.

The tab label keeps the stable Rumble channel name. While that channel is live,
the split header can also show the current stream title when **Appearance >
Split header > Title** is enabled. Title changes update only the header; they do
not rename a single- or multi-channel tab. The header drops a prior title after
the channel is confirmed offline or becomes unresolved, then adopts the next
stream's title after reconnection.

## Anonymous and authenticated behavior

Public resolution, history, realtime messages, links, badges, native emote
images, roles, deletes, pins, reconnect, and offline rechecks work without an
account. An exact emote shortcode renders only when the current stream's public
catalog defines it; ordinary or unknown colon text stays text. Unknown event
shapes are ignored with a sanitized diagnostic instead of being guessed.

Clicking a Rumble author opens a provider-scoped, minimal usercard. It shows the
packaged Rumble badge, the authoritative display/login name and user ID when
present, plus role, badge, and color metadata already carried by that message.
It deliberately does not look up a same-named Twitch or Kick account and does
not show their avatar, account metadata, profile links, decorations, notes, or
moderation controls. In a mixed multi-channel tab, the clicked message—not the
active child or selected account—owns the usercard identity and recent-message
filter.

Rumble author names likewise use only Rumble's color and badge metadata. A
same-named Twitch or Kick identity never lends its 7TV username paint to a
Rumble message, including the current account's newly sent messages. This does
not remove supported emotes from the message body; content remains governed by
the destination's normal completion and routing rules. After Rumble confirms a
send, Chatterino displays one provider-ID-bound optimistic message. The matching
realtime event or reconnect bootstrap replaces that exact row with authoritative
color, badge, role, and emote metadata; either network ordering remains
duplicate-free.

To add an account, open **Settings > Accounts**, choose **Add**, and select the
**Rumble** tab. Chatterino checks for Firefox, Chrome, Chromium, Edge, or Brave.
Selenium Manager obtains a matching browser driver when necessary and displays
an indeterminate progress bar while that one-time download is active. Select
**Log in to Rumble**, complete Rumble's normal login in the dedicated portrait
window, and wait for that window to close automatically.

The temporary browser uses an isolated profile and exits with its matching
driver before Chatterino accepts the result. Chatterino validates the session,
stores the password-equivalent value in the operating system credential store,
and writes only the account ID, display name, and selected-account ID to normal
settings. There is no plaintext fallback. If the platform keyring is
unavailable or denies access, the account is not added.

Chromium-family browsers use a fitted app-mode window without tabs or an
address bar. It remains movable and resizable and is never forced fullscreen.
Firefox does not expose a supported app-mode equivalent, so Chatterino prefers
an installed Chromium-family browser and otherwise uses an isolated, fitted
normal Firefox window with its ordinary browser chrome as the safe fallback.

On Linux, the credential store requires a running Secret Service provider such
as GNOME Keyring or KWallet. On NixOS, GNOME Keyring can be enabled with
`services.gnome.gnome-keyring.enable = true;`.

Use the person menu's permanent **Rumble** tab to select a saved account or
**Anonymous**. Use the shared **Remove** button on **Settings > Accounts** to
delete an account and its credential. Multiple Rumble accounts may be saved;
restarting Chatterino reloads the selected account without opening a browser.
Replacing, rejecting, removing, or shutting down the active session wipes its
in-memory credential buffer. Never paste or share a Rumble session value.

After validation, Chatterino submits each message at most once: it does not
retry an ambiguous delivery, and it preserves the draft after a definite or
ambiguous failure.

A validated selected account also enables Rumble-native completion. Global
entries are offered when usable by that account. Channel entries appear only
after Chatterino has established follower/subscriber/administrator eligibility
for the current live stream; locked or unknown candidates are hidden. Selecting
one inserts Rumble's ordinary colon-wrapped shortcode and sends through the
same at-most-once path. Changing account or stream invalidates the eligibility
snapshot. Multi-channel routing and its platform override honor the candidate's
Rumble account and channel scope. Provider-neutral Unicode emoji remain
available while anonymous.

Outbound moderation is explicitly unsupported. Authentication or an observed
role never enables delete, pin, mute, ban, or other moderation requests. The
accepted inbound-only contract is documented in
[`rumble-moderation.md`](rumble-moderation.md).

## Local status

Run `/rumble-status` in the Rumble split you want to inspect. The command emits
one block marked `safe to share`. It contains only fixed categories, bounded
counters/waits, and a UTC error time. It never contains the channel slug,
resolved IDs, account/user/message/request identity, username, URL, query,
header, cookie, request/response body, credential, or exception text.

| State | Meaning | Next action |
|---|---|---|
| `offline` | Resolution succeeded, but no live stream/chat exists. | Wait for the bounded recheck or retry manually. |
| `connecting` | Public resolution or the SSE connection is active. | Wait. |
| `connected` | A valid SSE `init` was accepted. | None. |
| `backoff` | A bounded automatic read retry is scheduled. | Wait for `retry-wait-ms`, or retry manually. |
| `rate-limited` | Provider policy delayed the retry. | Wait for `retry-wait-ms`; do not repeatedly retry. |
| `error` | Automatic attempts reached their bound. | Retry manually; include `/rumble-status` output in a bug report. |
| `stopped` | The channel/provider is closed and owns no active request/timer. | Reopen the channel. |

The online indicator records the last provider-confirmed stream availability,
not whether the long-lived SSE socket is connected at that instant. A normal
disconnect can therefore show `backoff` while the tab remains online and chat
temporarily becomes non-writable. Chatterino revalidates the public channel
after a stream-end-shaped terminal; only confirmed offline evidence changes
the tab to offline, removes Rumble completion/routing eligibility, and tells
the optional Streamlink follower to stop. The existing 30-second offline
recheck can restore the same tab when a new stream becomes live.

The account field is `logged-out`, `needs-validation`, `validating`, or
`authenticated`. The send contract supports selected saved accounts, so this
guide does not invent an `unsupported` account state. A rejected validation or
401 send returns the session to a safe logged-out or needs-validation state;
a stream-specific 403 keeps the validated session but disables that
destination. The `write` field is `unavailable`, `writable`, `busy`,
`rate-limited`, or `destination-denied`. A session-level 429 reports a bounded
`retry-wait-ms`; do not re-add the account or repeatedly retry to bypass it. A
destination-denied result applies only to that stream and directs sending to
Rumble's interface.

Rumble state transitions also use the dedicated `chatterino.rumble` logging
category. Records contain the same closed vocabulary as `/rumble-status`.
Equivalent repeated records are coalesced for a bounded interval with a bounded
key set and global emission budget. Adversarial key churn cannot bypass that
budget; the next emitted record reports only the numeric suppression count.

## Common problems

- **Response contract was not recognized while adding a channel:** no Rumble
  split exists yet, so `/rumble-status` cannot inspect it. Run the credential-free
  resolver from the repository root and paste its complete sanitized block:

  ```sh
  nix run 'github:the-fpp/klatterino#rumble-diagnose-channel' -- \
    --channel CHANNEL_SLUG
  ```

  Do not attach a HAR, packet capture, response body, private URL, cookie, or
  browser session.
- **Offline:** verify that the public channel is currently live. Offline is a
  normal resolved state and is automatically rechecked at a bounded cadence.
- **Backoff or rate-limited:** wait for the displayed delay. Repeated manual
  retries cannot bypass provider policy.
- **Needs validation or logged out:** select another saved Rumble account, add
  the account again if Rumble expired it, or choose Anonymous for read-only use.
- **Message may have sent:** check chat before trying again. Chatterino does not
  retry an ambiguous send.
- **Unsupported moderation action:** use Rumble's own interface. Chatterino has
  no accepted outbound moderation contract.

## Deterministic maintenance and external validation

Fixture replay, protocol refresh, parser, transport, lifecycle, and diagnostic
tests are offline and deterministic. See
[`tests/fixtures/rumble/README.md`](../tests/fixtures/rumble/README.md),
[`rumble-protocol.md`](rumble-protocol.md), and
[`rumble-lifecycle.md`](rumble-lifecycle.md).

From the repository root, the focused diagnostics suite is:

```sh
cmake --build build-test --parallel "$(nproc)"
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='RumbleDiagnostic*:*RumbleRedaction*:*RumbleStatusCommand*:*RumbleLogCoalescing*'
ctest --test-dir build-test --output-on-failure
```

The packaged public and authenticated validators are documented in the
repository root README. They are separate, explicit operator checks and are not
run by `/rumble-status`, normal startup, or CI. Their output is sanitized;
credentials and raw production payloads remain outside the repository.
