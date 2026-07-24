# Rumble public connection lifecycle and scheduler

The Rumble provider resolves only documented public locators and owns at most
one anonymous SSE request per channel lifecycle. It sends no credentials; the
request carries only Rumble's fixed public root Origin/Referer context observed
in the anonymous web client. It does not infer heartbeat behavior or probe the
live service in tests.

## States and retry policy

`Unresolved` resolves the public page/embed metadata, `Offline` schedules a
30-second recheck, `Connecting` owns an SSE request, and `Connected` is entered
only after a typed `init` event. Retryable read failures enter `Backoff` with a
copied lifecycle snapshot. A valid `Retry-After` is honored; otherwise full
jitter starts at one second and is capped at 30 seconds. The third consecutive
failure enters `Failed`. A valid `init` resets the counter, and explicit retry
starts a new generation and resolves the public locator again. `Closed` owns no
request, timer, callback, or reconnect delegate.

The snapshot exposes only typed retry causes, monotonic scheduled/deadline
milliseconds, the failure count, and a rate-limit flag. It never stores URLs,
headers, response fragments, provider IDs, or Qt error strings.

## Bounds and ordering

- Final response headers retain the normal 20-second deadline. A validated SSE
  head ends that deadline; there is deliberately no idle/heartbeat timeout.
- The Qt adapter bounds bytes and chunks pending consumer delivery. Delivered
  bytes are released from that accounting, so a valid long-lived stream has no
  cumulative lifetime-byte ceiling.
- SSE records, typed batches, queued handoffs, user/channel/badge catalogs, and
  message-ID deduplication all have deterministic finite bounds.
- Reconnect init history uses `Channel::fillInMissingMessages`, preserving
  chronological order and suppressing overlap with earlier history/realtime
  messages.
- Lifecycle, request, and timer generations are rechecked after every channel
  publication. Synchronous signals may request reconnect, but a stale handler
  cannot continue publishing or install a newer operation.

All verification uses the scripted fixture transport, fake monotonic time, and
injected randomness. No wall-clock sleep or production Rumble traffic is part
of the test contract.

## Ownership and cancellation

`RumbleConnection` owns resolution, one SSE reader, and one scheduled retry for
one channel operation generation. The channel owns a small cancellation
adapter, not the Qt objects themselves. Cancellation first closes an atomic
callback gate on the calling thread. It then transfers API/timer cancellation
and destruction to the dispatcher owner thread. Lifecycle, request, timer, and
external-restart generations make callbacks from a cancelled generation inert,
including a deliberately fired cancelled fake timer.

`RumbleScheduler` never invokes a callback inline. `QtRumbleScheduler` uses a
monotonic `QElapsedTimer`, injectable randomness, and single-shot `QTimer`
chunks. Delays larger than `INT_MAX` milliseconds are rearmed in monotonic
chunks, so every `Retry-After` value accepted by the API remains schedulable.
The scheduler owner must outlive its connection controllers.

## Deterministic verification

The fixture suite covers public page/embed resolution, direct stream IDs,
offline/204 behavior, disconnect recovery, clean EOF, HTTP 408/429/5xx and
non-retryable failures, valid and invalid `Retry-After`, jitter growth/cap,
attempt exhaustion, manual retry, context replacement, dedup overlap, hostile
IDs, reentrancy, cancelled stale timers, off-owner cancellation, and provider
shutdown. Qt adapter tests separately exercise pending byte/chunk release,
overflow, cancellation, and final-head-only deadlines.

```sh
cmake --build build-test --parallel "$(nproc)"
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='RumbleConnection*:*RumbleLifecycle*:*RumbleReconnect*:*RumbleDedup*'
ctest --test-dir build-test --output-on-failure
```
