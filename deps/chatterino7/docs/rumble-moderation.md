# Rumble moderation capability contract

Rumble moderation is an inbound-only, unstable capability. The accepted
fixtures expose deletion and pin state, and the preserved experiment mentions
mute operations, but there is no frozen accepted outbound request or
authorization-error contract. Chatterino therefore must not make a mutation
available merely because a session is authenticated or a role label was seen.

## Identity and reducer rules

- Account, channel, user, and message IDs are opaque normalized strings.
  Display names and numeric conversions are never identities.
- One `ModerationState` belongs to exactly one `(optional account ID, channel
  ID)` pair. Anonymous streams have no account identity.
  Cross-scope events are rejected and never mutate state.
- The connection assigns a local arrival revision; no event ID or revision is
  claimed to exist on the wire. Revisions are strictly increasing, so replayed
  or reordered older queued operations are ignored. A bounded semantic replay
  horizon is twice the bounded delete state capacity, so a delete replay
  remains inert after its oldest visible tombstone has been evicted. Beyond
  that explicit horizon, accepted wire data is reduced again because it has no
  event ID. State-setting pin, mute, and role updates use ordered current-state
  comparison so legitimate transitions back to an earlier value remain valid.
- A typed role snapshot replaces prior roles. Wire role/badge strings remain
  uninterpreted unless an accepted fixture adapter establishes their meaning.
  A role never grants a network capability;
  it only distinguishes an unauthorized account from a privileged account for
  which the mutation is still explicitly unsupported.
- Delete, pin/unpin, and mute/unmute events reduce deterministic local state.
  This DTO boundary does not guess any additional wire event shape.

## Capability matrix

| Capability | No accepted role | Broadcaster/moderator role | Status |
|---|---|---|---|
| Observe accepted delete events | available | available | unstable inbound |
| Observe accepted pin events | available | available | unstable inbound |
| Reduce typed mute events | available | available | unstable inbound DTO only |
| Delete message | unauthorized | unsupported | no accepted outbound contract |
| Pin/unpin message | unauthorized | unsupported | no accepted outbound contract |
| Mute/unmute user | unauthorized | unsupported | no accepted outbound contract |
| Ban/unban user | unauthorized | unsupported | no accepted inbound or outbound contract |

Unknown role and badge strings are preserved by the event catalog but are not
interpreted as privileges. Subscriber and every non-moderator fixture role are
non-authorizing. Account or channel changes require a new reducer and
capability snapshot. Authentication alone never implies moderator status.

Deterministic coverage is in `tests/src/RumbleModeration.cpp`:

```sh
build/bin/chatterino-test \
  --gtest_filter='RumbleModeration*'
```
