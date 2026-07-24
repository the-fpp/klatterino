# Rumble typed read-only boundary

`providers/rumble/RumbleApi.*` implements the public locator, resolution, and
bounded anonymous persistent typed-event stream boundary described by
[`rumble-protocol.md`](rumble-protocol.md). It does not reimplement the event
payload schema: complete SSE records are delegated to #10's `EventParser`.
`bootstrap()` is a compatibility wrapper over `stream()` which cancels after
the first typed batch. The API does not implement reconnect policy,
channel-state application, authentication, chat mutations, or moderation.

## Ownership and callback contract

- `RumbleApi`, its injected `Transport`, and the deferred dispatcher must
  outlive every outstanding `Cancellation`.
- `Cancellation` is move-only and RAII-cancels. Explicit cancellation, API
  destruction, transport destruction, and owner destruction abort work and
  suppress queued consumer callbacks. Network-manager destruction instead
  stops and retires its manager-owned work while suppressing callbacks.
- A transport may complete synchronously, but `RumbleApi` schedules typed
  batches and the exactly-once terminal callback through the injected
  dispatcher. Batches are always delivered before the terminal. The
  cancellation handle is returned before consumer code can run.
- `RumbleQtTransport` is single-thread-affine. Construct, start, and cancel it
  on its QObject thread. Its head, body, and terminal callbacks are queued on
  that thread. Its default `QNetworkAccessManager` is dedicated; an injected
  manager and every reply it returns must share the same thread. A reply on
  another thread fails closed, with cleanup queued on that reply's thread.
- Qt exposes no pre-destruction hook for an injected manager. Its teardown
  synchronously destroys manager-owned replies; their destruction hook silently
  retires transport handles and suppresses callbacks without calling `abort()`.
- User cancellation is silent. A remote cancellation not requested by the
  owner is a typed `Cancelled` outcome.

## Locator and parser policy

Only the documented channel/profile, video/embed, video-page, and positive
decimal stream-ID forms are accepted. Host, path, query, fragment, userinfo,
port, and percent-encoding rules are canonical and fail closed.

Page extraction uses a bounded tag and exact-attribute scanner. It ignores
comments, script strings, inert template/noscript/style/textarea/xmp/noembed/
noframes content, everything after a `plaintext` start tag, `data-src`, and
`data-type`; only a real iframe `src` or explicitly typed JSON script can
supply an embed URL. Page bodies remain capped at 4 MiB and are closed earlier
when an authoritative prefix is complete. Within that bound, the
outer scanner skips unrelated hydration, layout, and inline-SVG tags without
parsing their attributes, so their size or duplicate inert attributes cannot
change the resolver contract. Template contents retain exact tokenization so a
fake close inside inert markup cannot escape that scope. Pre-body title,
interstitial form, template/raw-text, iframe, and typed-script tags retain the
exact 4 KiB tag and unique-attribute checks. Only pre-body HTML document titles
participate in page identity; body titles, including inline-SVG accessibility
labels, are consumed as inert raw text and cannot create a false ambiguity.
Multiple pre-body document titles remain malformed.
A channel index's first outer `videostream` card is the live-state authority:
an exact zero duration is live, while a positive decimal duration is offline.
The marker is the exact `duration` attribute; a missing, duplicated, negative,
or non-canonical value on that first card is malformed. Later cards cannot
override it, and an offline card wins over stale embed metadata retained for
the most recent recording. Pages without a `videostream` card retain the older
embed-only fallback for compatible direct video and synthetic fixture
resolution.
A typed JSON script with duplicate decoded object keys is ignored without
blocking a valid source elsewhere on the page. Duplicate attributes on a
contract-bearing tag, ambiguous embed IDs, unterminated markup, oversized
contract-bearing tags, and invalid UTF-8 are malformed input.
For non-void ignored elements and iframes, a trailing slash does not close the
HTML element: the scanner consumes through a matching end tag or fails closed.

Embed JSON is parsed twice: Qt validates the typed object, while a bounded
top-level token scanner preserves decimal identifiers exactly. Nested keys
cannot shadow top-level IDs, and duplicate decoded top-level keys—including
escaped spellings—are rejected. Direct video and video-page resolution does
not invent channel identity or title; unavailable fields remain empty unless
the documented response supplies them.
After JSON structure and duplicate-key validation, an absent or null top-level
`vid` is a valid offline result. A present `vid` with the wrong type or a
non-positive/non-lossless value remains malformed.

## Network policy

The Qt adapter accepts only these HTTPS shapes:

- `rumble.com/c/<slug>/live/`;
- `rumble.com/user/<slug>/live/`;
- one accepted `rumble.com/v…html` video page;
- `rumble.com/embedJS/u3/?request=video&ver=2&v=<embed-id>`; and
- `web7.rumble.com/chat/api/chat/<decimal-id>/stream`.

It manually follows at most three redirects that keep the exact canonical
endpoint path/query and therefore cannot change a channel slug, video/embed
ID, stream ID, or endpoint family. Header and body limits apply independently
to every response in the chain; redirect payloads are counted and discarded.
Requests reject unapproved or duplicate headers and never set Cookie or
Authorization. Anonymous SSE sends only the fixed public browser context
observed on the credential-free watch page: `Origin: https://rumble.com` and
`Referer: https://rumble.com/`. Arbitrary origins, identifying referrers, and
missing context headers fail closed. Qt cookie load/save and authentication
reuse are `Manual`; cache loading is `AlwaysNetwork`, cache saving is disabled,
and redirects are manual.

Finite requests use `WholeResponse` deadlines and `Cumulative` body limits: a
20-second deadline plus bounded headers/body. Persistent SSE uses
`UntilFinalHead` and `PendingDelivery`: the same 20-second deadline ends after
the validated final head, while bytes and chunk slots are released after
consumer delivery. A valid long-lived stream therefore has no cumulative
lifetime-byte limit or invented heartbeat timeout, but queued memory remains
bounded. Crossed scope pairs and persistent scopes on non-SSE requests fail
closed.

A successful representation must have its expected media type; non-2xx status
classification takes precedence over an error body's media type. SSE accepts
the bare `text/event-stream` type or the observed one/two identical
`charset=utf-8` parameters. Other parameters, non-UTF-8 charsets, more than two
charset parameters, and duplicate Content-Type fields fail closed. SSE permits
at most 64 pending transport chunks, 256 KiB pending bytes/incomplete tail, 64
complete records per typed handoff, 64 KiB of JSON data per record, and eight
queued typed handoffs. Queued reply bytes are drained before terminal
classification. HTTP 4xx/5xx remains an HTTP response even though Qt also sets
a reply error; a premature close of a nominal 2xx response is a retryable
network failure, and pre-head Qt/timer timeouts are retryable typed timeouts.

## Bounded SSE handoff

SSE is parsed incrementally across CRLF, CR, and LF chunk boundaries. Only a
blank-line-delimited event is eligible; an unterminated tail remains pending.
Each complete record is immediately passed through #10's `EventParser`.
Unknown or structurally invalid records produce sanitized diagnostics and are
skipped; recoverable field diagnostics accompany the typed event. None expose
their JSON or disconnect an otherwise healthy stream. Each non-empty
typed/diagnostic batch is queued through one bounded pump. `bootstrap()`
intercepts the first typed batch, cancels the persistent child immediately,
and defers its compatibility callback. Diagnostic-only batches are retained
only up to the record bound for that wrapper.

For `stream()`, a clean HTTP 200 EOF (with an empty, comment-only, or
incomplete tail) is a retryable `stream_eof` terminal after every complete
batch. HTTP 204 is a typed `ValidOffline` terminal. Cancellation is silent and
suppresses queued batches. For `bootstrap()`, EOF without a typed batch remains
the compatibility `sse_schema` result.

SSE syntax failures use the stable `sse_schema` code. The separate event-count
and per-event-size bounds are `LimitExceeded` results with
`sse_event_count_limit` and `sse_event_size_limit`, respectively.

Reconnect, backoff, and application of typed events to channel state are
documented in [`rumble-lifecycle.md`](rumble-lifecycle.md). No heartbeat/idle
policy exists until a future accepted contract defines an explicit signal.

## Sanitized outcomes

Public results contain typed outcomes, stable string identifiers, validated
titles, parsed retry metadata, #10 `Event` variants, and fixed diagnostic
codes/schema paths. They never expose a raw page/embed body, SSE field or JSON
record, response header, cookie, locator-bearing URL, Qt network error,
exception text, or `QNetworkReply`.

No automatic retry is performed. Retry metadata marks network failures,
timeouts, HTTP 408/429, and 5xx responses as retryable; cancellation, policy,
schema, and limit failures are not. `Retry-After` accepts decimal seconds or a
strict IMF-fixdate with valid calendar fields and matching weekday.

Finite page/embed requests use cumulative body limits. The persistent SSE
request instead uses its byte/chunk limit as a pending-delivery backpressure
window: the Qt reader pauses without failing and resumes after the queued body
callback drains. Parsed SSE records retain their independent bounded event
limit. Body-limit diagnostic codes are stage-specific (`page_body_limit`,
`embed_body_limit`, and `sse_body_limit`) so a stream failure cannot be
misidentified as a page-parser failure.

## Deterministic verification

The focused suite uses #18's scripted transport, synthetic resources, and a
fake `QNetworkAccessManager`/`QNetworkReply` that exercises the concrete Qt
adapter. It does not contact Rumble:

```sh
cmake --build build-test --parallel "$(nproc)"
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='RumbleApi*:*RumbleResolver*:*RumbleTransport*'
ctest --test-dir build-test --output-on-failure
```

Production endpoint validation belongs to the packaged #36 runner and is not
required to implement, test, review, or merge this boundary.
