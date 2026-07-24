# Rumble protocol fixtures

This directory contains deterministic, synthetic protocol inputs for offline
tests. The inputs preserve the raw form consumed at each boundary: channel-page
HTML, embed JSON, HTTP status and headers, SSE framing, and prescribed transport
chunk boundaries. Normal tests make no network requests and never sleep on a
wall-clock timer.

## Provenance

All files were hand-authored from the discovery contract in
`docs/rumble-protocol.md`, last reviewed 2026-07-12. They are shape examples,
not packet captures or copies of live responses. Public fixtures model the
anonymous read-only path. `session-expired.json` models only the shape of an
authenticated rejection and contains no credential or account data.

`provenance.json` is the machine-checked sidecar for every raw resource. It
records the review date, public/authenticated classification, method, statuses,
content type, replacements, and consuming tests. The table below is its
human-readable counterpart.

| Fixture | Classification and boundary | Replacements | Consuming tests |
|---|---|---|---|
| `raw/channel-live.html` | public `GET`, HTTP 200 `text/html`; first video card has duration zero and an embed reference | synthetic channel name and embed ID; unrelated scripts and tracking removed | `PreservesRawHttpAndSseBoundaries`, `DrivesCompleteRawLiveSession` |
| `raw/channel-offline.html` | public `GET`, HTTP 200 `text/html`; first video card has positive duration plus a stale embed reference | synthetic channel, duration, and embed ID; unrelated page content removed | `ScriptsOfflineAndHttpFailuresIndependently` |
| `raw/embed-live.json` | public `GET`, HTTP 200 `application/json`; positive `vid` | stream ID mapped to `1001`; title replaced | `PreservesRawHttpAndSseBoundaries`, `DrivesCompleteRawLiveSession` |
| `raw/embed-offline.json` | public `GET`, HTTP 200 `application/json`; unavailable embed | all identifiers removed | `ScriptsOfflineAndHttpFailuresIndependently` |
| `raw/sse-live.txt` | public `GET`, HTTP 200 `text/event-stream`; current array-valued user/channel catalogs, `time` timestamps, init, role/message delta, and deletion | stable test-only user, channel, and message IDs; synthetic text and timestamps | `PreservesRawHttpAndSseBoundaries`, `DrivesCompleteRawLiveSession` |
| `raw/sse-state-events.txt` | public `GET`, HTTP 200 `text/event-stream`; clear-non-rant and pin operations | stable test-only IDs, text, and timestamps | `RumbleEventParser.ParsesStateOperationFixture` |
| `raw/sse-reconnect-*.txt` | public `GET`, HTTP 200 `text/event-stream`; disconnect and repeated initial window | stable test-only IDs and messages | `ReconnectSequenceIsFiniteAndDeterministic` |
| `raw/sse-malformed.txt` | public SSE malformed JSON block | hand-authored invalid bytes | `PreservesAdversarialSseInputs` |
| `raw/sse-duplicate.txt` | public SSE duplicate message delivery | duplicated synthetic message ID | `PreservesAdversarialSseInputs` |
| `raw/sse-out-of-order.txt` | public SSE later message followed by earlier message | synthetic IDs, text, and ordered test timestamps | `PreservesAdversarialSseInputs` |
| `raw/sse-unknown.txt` | public SSE unknown event type | synthetic event name and payload | `PreservesAdversarialSseInputs` |
| `raw/sse-delayed.txt` | public SSE message used by virtual-time and lifetime scripts | synthetic IDs, text, and timestamp | `DelayedDeliveryUsesOnlyVirtualTime`, cancellation and stale-generation tests |
| `raw/http-error.json` | public HTTP 403/404/429/503 `application/json` shape | generic synthetic error text | `ScriptsOfflineAndHttpFailuresIndependently` |
| `raw/session-expired.json` | authenticated HTTP 401 `application/json` rejection shape | account, cookie, and server detail omitted | `ScriptsOfflineAndHttpFailuresIndependently` |

## Script and transport model

`scenarios.json` is the versioned replay manifest. Each named scenario contains
one or more finite exchanges:

- the exact expected method, target, and required request headers;
- a response head with status and headers;
- zero or more raw resource slices, each with a virtual delay relative to the
  preceding boundary; and
- a terminal completion or disconnect with an optional virtual delay.

Repeated slices of `sse-live.txt` deliberately split the first SSE block after
`data: ` and again inside its JSON. This lets downstream parsers prove that they
handle incremental bytes instead of accidentally relying on event-aligned
network reads.

The manifest provides independent scenarios for live bootstrap, offline page,
SSE 204, unavailable embed, reconnect, 401, 403, 404, 429 plus `Retry-After`,
503, malformed input, duplicate input, out-of-order input, unknown events,
delayed delivery, stale completion, and cancellation.

`tests/src/lib/RumbleFixtureTransport.*` is the reusable test-only transport.
`ManualScheduler` delivers equal-deadline work in insertion order and advances
only when a test calls `runReady`, `advanceBy`, or `runUntilIdle`. Request
handles cancel on destruction. Cancellation and transport destruction remove
all scheduled work and release callbacks before the consumer can be destroyed.

## Sanitization rules

- Use only clearly synthetic channel, user, message, request, and stream IDs.
- Replace display names, titles, and message text with fixture-specific values.
- Never include cookies, authorization values, creator API URLs, stream keys,
  account email or phone values, 2FA fields, IP addresses, private messages,
  identifying broadcast timestamps, browser IDs, or reversible hashes of real
  identifiers.
- Preserve only protocol shape, status/header semantics, byte framing, chunk
  order, and timing relationships required by a test.
- Do not commit raw originals, packet captures, or a live response before
  sanitization.

## Refresh procedure

1. Compare a purpose-made public test observation with
   `docs/rumble-protocol.md`; never copy the raw observation into the repository.
2. Hand-author the smallest synthetic raw fixture that preserves the changed
   boundary and map every identifier consistently to documented test values.
3. Add or update one named finite scenario in `scenarios.json`.
4. Update the provenance table with classification, method/status/content type,
   removed or replaced fields, review date, and consuming test.
5. Run the focused `RumbleFixture*` tests and the normal Chatterino test target.
6. Scan the complete fixture and documentation diff for secrets, URLs, IPs, and
   identifying values. Review matches in prose; fixture payloads must have none.

One suitable targeted scan is:

```sh
git diff --cached -- deps/chatterino7/tests/fixtures/rumble \
  | rg -n 'u_s|cookie:|authorization:|stream[_-]?key|api[_-]?key|https?://|([0-9]{1,3}\.){3}[0-9]{1,3}'
```

The synthetic public embed URLs in `channel-live.html` and
`channel-offline.html` are expected reviewed matches. No other fixture payload
should match.
