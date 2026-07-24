# Rumble channel ownership and mutation contract

RumbleChannelProvider owns a dispatcher and only weak cache/alias entries.
Callers own channels. Each channel receives a collision-resistant,
filesystem-safe `rumble-<uuid>` identity that remains stable for that channel
object and is independent of every provisional or canonical Rumble locator.
Raw locator text is used only in typed cache keys and user-facing display
metadata. Both display
name APIs return that metadata (or the initial locator before resolution), so
the opaque identity stays in logging and draft contexts instead of leaking into
tabs or headers. Callbacks capture a weak channel and a typed operation token.
A channel owns resolver and connection handle slots.
beginOperation
invalidates and cancels the previous same-kind handle before returning a new
monotonically increasing generation. RumbleChannelOperation::cancel must be
thread-safe, idempotent, noexcept, and suppress later publication. Successful
completion calls release before destroying the handle, so normal completion is
observable separately from cancellation; release and cancel are mutually
idempotent.

All observable mutation runs on the injected dispatcher owner thread. An
off-thread typed callback queues exactly one FIFO task. On execution it verifies
owner affinity and token generation. The provider's shared-pointer deleter
synchronously gates publication and cancels transport work, then transfers the
raw channel to the dispatcher's separate owner-affine disposal path. The Qt
dispatcher uses a private, unparented receiver on the owner's thread; each
cleanup has copyable ownership that releases on execution or queued-event
destruction. Ordinary dispatch rejection and destruction of the external
context QObject therefore cannot destroy Channel's QTimer or ChannelChatters'
QObject on a producer thread, or strand their channel. Tests inject a
thread-safe FIFO dispatcher and use explicit handshakes rather than sleeps,
timers, or network.

State and copied `RumbleLifecycleMetadata` are committed as one atomic snapshot
on the owner. Metadata contains only a failure count, monotonic
scheduled/deadline milliseconds, a rate-limit flag, and a typed sanitized retry
cause. `stateChanged`, the matching `liveStatusChanged`, and
`lifecycleMetadataChanged` observe that committed snapshot before another
typed transition may mutate it. Reentrant typed transitions receive
`ReentrantTransition`; a reentrant void close is deferred until the current
signal sequence finishes. Invalid/reentrant transitions cannot partially
change metadata, and the Closed snapshot clears every pending deadline.

Allowed transitions:
Unresolved -> Offline, Connecting, Backoff, Failed, Closed
Offline -> Unresolved, Closed
Connecting -> Connected, Offline, Backoff, Failed, Closed
Connected -> Offline, Backoff, Closed
Backoff -> Unresolved, Connecting, Failed, Closed
Failed -> Unresolved, Closed
Closed -> none
Same state is an idempotent success. Rejections are typed and non-mutating.
Failure accepts only enum category/code and an optional enum selecting a
compiled-in operator-safe sentence; no raw diagnostic string is accepted.

close is idempotent: set Closed, invalidate tokens, remove reconnect capability,
cancel handles, and make queued/future publication inert. An atomic closing gate
is set synchronously even when close is requested off-owner, so work queued both
before and after that request is discarded before the queued owner-thread state
transition runs. Reconnect availability remains observably unchanged until that
owner transition and its ordered signal. Transport handles are cancelled
immediately through their required thread-safe interface; the handles and any
retired reconnect callable are then transferred to owner-affine disposal before
their destructors run. If dispatch rejects because the owner is gone, close
performs a signal-free terminal fallback so no operation remains live and no
controller cycle stays in the channel. Destruction does the same without
signals or application singletons. Provider shutdown
takes unique temporary strong snapshots, clears cache/aliases, and closes each
once. Construction performs no network I/O, timer start, task, or publication.
Sendability remains false until #20.

#9 locator -> RumbleChannelKey at provider entry and cancellation -> operation
adapter. #10 MessageDto -> RumbleMessagePublication::fromDto, which uses the
existing message builder and preserves MessageDto::rant; delete IDs, clear
mode, and pin become typed operations. Reconnect history uses
`fillInMissingMessages()` so an init replay is chronological and ID-overlap
safe. #22 owns the connection, timer, reconnect/backoff, and thread-safe
cancellation adapter described in
[`rumble-lifecycle.md`](rumble-lifecycle.md). #19 owns
application/picker/layout. #12 owns moderation; #13 and #101 own asset and
native-emote behavior.
