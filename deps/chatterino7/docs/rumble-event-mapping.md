# Rumble event mapping

This is the closed, fixture-backed mapping implemented by issue #10. Raw JSON
ends at `rumble::EventParser`; connection and channel owners consume the typed
variants and `rumble::EventState` output.

| Payload | Typed result | Chatterino effect in this issue |
|---|---|---|
| `init` | `InitEvent` with user/channel/badge deltas and messages | Apply deltas, timestamp/arrival sort the bootstrap, FIFO-deduplicate, then build messages |
| `messages` | `MessagesEvent` with deltas and messages | Apply deltas, preserve realtime arrival order, FIFO-deduplicate, then build messages |
| `delete_messages` | `DeleteMessagesEvent` | Emit a typed state operation; #11/#12 owns channel mutation |
| `delete_non_rant_messages` | `DeleteNonRantMessagesEvent` | Emit typed IDs/clear intent; #11/#12 owns channel mutation |
| `pin_message` | `PinMessageEvent` | Emit a hydrated typed pin operation; #11/#12 owns channel mutation |

Message IDs accept non-negative JSON integers or non-empty normalized strings.
String IDs with leading or
trailing whitespace, NUL, or control characters are rejected identically by
the parser and channel-publication boundary. User and channel identifiers
remain strings. `created_on` retains its source text and is also parsed to a
UTC instant. Missing presentation fields use neutral
identity/color/badge/role/source defaults when state is hydrated.

`EventState` preserves its message-ID FIFO across reconnects to the same stream
and resets it on stream identity change. The FIFO has a fixed capacity and
deterministic oldest-entry eviction. User, channel, and badge presentation
catalogs are independently capped at that configured capacity. User/channel
deltas are applied in sorted-ID order before FIFO eviction; each init replaces
the badge catalog with the sorted bounded prefix. Hydrated messages copy badge
titles, so later catalog eviction cannot mutate already-published messages.

Intentionally unsupported event types are every type not listed above. They
produce only the stable `unknown_event_type` diagnostic. The parser does not
guess reply, rant-payment, moderation, unrecognized emote/image metadata,
reconnect, or mutation
shapes, and diagnostics never contain source payload values.

The implementation is pure and offline: it owns no socket, timer, network
reply, reconnect loop, channel, or wall-clock wait.
