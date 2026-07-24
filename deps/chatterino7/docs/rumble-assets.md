# Rumble assets

Rumble chat has accepted graphical-badge and native-emote contracts. Badges
come from the anonymous `init` event. Native emotes come from the public,
bounded `emote.list` catalog for the resolved live stream.

## Graphical badges

A Rumble badge's stable identity is the pair `(rumble, exact badge ID)`. The
badge label and icon URLs are presentation metadata, not identity. They can
change while an ID remains the same.

An `init` event replaces the active badge catalog with
`data.config.badges`. Replacement also occurs when the accepted catalog is
empty. Changing stream identity clears the catalog before any event for the new
stream is hydrated. Each hydrated message snapshots its resolved badges, so a
later `init` cannot rewrite an already-built message.

Badge membership comes only from accepted user or message `badges` arrays of
strings. Message membership takes precedence over hydrated user membership,
including an explicitly empty message array. User deltas replace the user's
badge IDs for subsequently hydrated messages. Roles are separate metadata and
never create badges.

Empty badge IDs are ignored. A known ID uses its non-empty string `title`, or
the observed `label.en` string when no title is present. An unknown ID, missing
label, empty label, or label of the wrong type retains the exact ID as its
metadata title. The title is HTML-escaped before it is stored as a rich
tooltip.

Static badge images come only from the catalog's `icons["48"]` and optional
`icons["96"]` fields. Relative paths are resolved against `https://rumble.com`.
An image URL must be HTTPS, have no query or fragment, use a known Rumble asset
host, and begin with `/i/badges/`. Invalid URLs are discarded rather than
loaded.

A badge with an accepted image renders as an 18-pixel `BadgeElement` between
the timestamp and username. Authority, subscription, and vanity IDs use their
matching Chatterino badge visibility categories. Iconless and unknown badge
IDs remain available in provider metadata but have no bracketed-text fallback.
The shared image cache loads a URL only when normal message rendering needs it.

## Native emotes

The current public Rumble client requests
`/service.php?name=emote.list&chat_id=<decimal stream ID>`. Its response groups
emotes by an exact group ID and nullable `channel_id`. A null channel identifies
the public global group; observed names in that group use the `r+` prefix.
Non-null groups are destination-channel emotes and their observed names do not
use `r+`. Thus `r+` means the name belongs to Rumble's global catalog; it is not
itself a subscription or premium marker.

Each accepted entry supplies `name`, integer `position`, `file`, and boolean
`is_subs_only`. Chatterino's stable provider identity is
`(rumble, exact group ID + position)`. Name, image, scope, and eligibility are
mutable metadata. This prevents a renamed or replaced asset from changing the
identity of already-built messages.

The web client recognizes only an exact, case-sensitive, colon-wrapped token
present in that catalog: `:r+name:` for global entries or `:name:` for channel
entries. It inserts and sends that same ordinary text representation; there is
no structured outbound encoding. Chatterino follows that contract. Catalog
membership is authoritative, so unknown or malformed colon text remains
literal text. Message `blocks` do not supply emote ranges; occurrences are
therefore derived with the web client's catalog-and-token rule in QString's
UTF-16 units. Each hydrated message snapshots its resolved definitions and
occurrences before rendering normal `EmoteElement`s.

The public catalog is fetched once after resolving each stream and again on a
read reconnect. A stream identity change clears the old catalog. Requests are
limited to 1 MiB; parsing accepts at most 64 groups, 4096 definitions, 64
characters per name, 2048 bytes per URL, and 256 occurrences per message.
Images must be HTTPS on `1a-1791.com`, have no credentials, port, query, or
fragment, and use the observed `/video/z12/` path. They load through
Chatterino's shared image cache only when normal rendering needs them.

Incoming rendering is anonymous. Completion is stricter: a selected account
must have a validated session. Global non-subscription entries are then
available. Channel entries require the current account to be a follower,
subscriber, or administrator of that destination, and `is_subs_only` entries
require a subscription-supporter or administrator badge. The eligibility
snapshot comes from the authenticated stream's initial user catalog, is cached
outside the keystroke path, and is invalidated with the session or stream.
Unknown eligibility hides the candidate. Vanity/premium presentation badges do
not independently grant emote access.

Completion retains provider identity plus exact account/channel scope. This
allows single- and multi-channel routing, automatic selection, and explicit
platform override to use the existing exactly-once text send without guessing
a destination. Anonymous Rumble channels still expose provider-neutral Unicode
emoji.

## Tests

The deterministic offline contract is covered by `tests/src/RumbleAssets.cpp`,
`tests/src/RumbleEmotes.cpp`, and the Rumble session/routing suites:

```sh
cmake --build build-test --parallel "$(nproc)"
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='RumbleBadge.*:RumbleAsset.*:RumbleEmote*.*:RumbleAuth*.*:RumbleMultiChannel*.*'
ctest --test-dir build-test --output-on-failure
```

The focused suites validate image identity without fetching the image, so they
require no live channel, account secret, browser session, or remote asset
server.
