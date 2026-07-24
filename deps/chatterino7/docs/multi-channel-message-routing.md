# Multi-channel message routing

Ordinary messages in a multi-channel split are sent to exactly one child. The
router captures one immutable send-context snapshot for every child and then
applies these rules in order:

1. Reject children that fail a hard requirement: writability, authentication,
   provider length/format constraints, malformed provenance, or an unsupported
   explicit completion/picker selection.
2. Count the emote occurrences each remaining child can render. Repeated
   occurrences count separately.
3. Choose the child with the greatest count.
4. Break a count tie with the primary child, then newest live-chat activity,
   then stable child order.

An emote-like word inferred from ordinary typed text is soft provenance. If the
chosen child cannot render it, the original token is sent unchanged as text.
Selections made through completion or the emote picker are hard provenance and
remain restricted to destinations that support the selected provider identity.
When one destination supports every occurrence, that destination therefore
beats any partial match.

Provider commands and replies use primary-only routing and never fall back to a
different child. Full `https://7tv.app/emotes/<id>` values are links, not emote
provenance, and are sent unchanged.

## Pending destination and platform override

Every multi-channel message input shows the platform badge for the destination
the same router would currently select. An empty or plain tied draft shows the
persisted primary only while that child is eligible; otherwise it follows the
same activity and stable-order fallback used by submission. If no child is
eligible, the badge is cleared and exposes an explicit unavailable description
instead of falling back visually to a child that cannot accept the draft. As
the draft changes, the badge is recomputed from a side-effect-free snapshot of
the same child contexts used by submission.

The badge menu can constrain that one input to Twitch, Kick, or Rumble without
changing the aggregate's active child or another view of the aggregate. While
an override is active:

- the badge has a red outline;
- completion exposes only emotes available through an authenticated, writable
  child of the selected platform;
- submission excludes every other platform before applying the normal primary,
  live-activity, and child-order tie breakers; and
- an emote selected before the override is rendered as ordinary text when the
  chosen platform cannot provide it.

In a focused multi-channel input, Tab cycles through automatic routing and each
distinct currently eligible platform, wrapping at either end; Shift+Tab cycles
backward. The choice is temporary for the current message and is marked with a
small `1` badge. It also controls routing preview, completion sources, and
validation exactly like a menu override. A successful send clears the temporary
choice, while a rejected or failed send retains it with the draft. The platform
picker remains available while its popup is open, and a persistent menu override
is restored after the temporary choice is cleared.

Commands and supported structured replies keep their provider-bound meaning.
If their bound child does not match the override, submission is rejected and
the draft is retained; it is never sent through another platform. A Rumble
reply remains bound to its selected child, but because Rumble has no structured
reply contract its complete visible text is dispatched once as an ordinary
message and the obsolete relationship is removed. Recoverable failures retain
that ordinary draft. Selecting **Automatic routing** clears the override.

Rumble has no offline chat destination. Its menu action is therefore enabled
only while at least one Rumble child has a live chat session. If the last such
child becomes definitively offline, an active Rumble override is cleared
immediately, the red outline disappears, and the unchanged draft returns to
automatic preview, completion, and reply routing. A recoverable reconnect or
backoff keeps the action available, as does a live anonymous/logged-out session;
authentication errors continue through normal send feedback. Automatic routing
also excludes the confirmed-offline Rumble child from its effective primary;
an eligible Twitch or Kick child can remain available even when its stream is
offline because those providers support offline chat. Eligibility transitions
refresh an already-open completion popup and its primary tier without changing
the persisted child order or the draft. When Rumble becomes eligible again,
the existing primary/tie policy applies immediately. Twitch and Kick menu
availability remains presence-based.

The pure tie-breaking contract is tested in `tests/src/MultiChannelRouting.cpp`.
Draft classification and hard constraints are tested in
`tests/src/MessageDraft.cpp`; the complete input-to-send path is covered by
`tests/src/MultiChannelCompletionRoutingRegression.cpp`.
