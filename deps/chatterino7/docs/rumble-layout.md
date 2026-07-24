# Rumble application and layout integration

Rumble views retain a canonical public locator independently of the runtime
channel object. Channel, embed, and public video-page inputs are normalized to
an `https://rumble.com/...` URL with query and fragment removed. Direct stream
IDs are transport identities and are deliberately rejected by the picker. The
picker also accepts a bare channel slug for convenience, but persisted data is
accepted only when it contains an explicit public HTTPS Rumble URL.

Window and multi-channel descriptors use the stable `rumble`/`Rumble`
platform identifiers plus `locator` and the equivalent validated
`layoutIdentity`. The encoder validates again at the final disk boundary. A
missing, foreign, malformed, or otherwise unsafe saved value restores as a
visible Rumble placeholder with no locator; the unsafe value and the provider's
`rumble-<uuid>` runtime name are never reserialized. A valid locator remains on
the placeholder when the provider is temporarily unavailable.

`RumbleApplicationController` owns the production transport, API, scheduler,
provider, and one bounded `RumbleConnection` lifecycle per provider channel.
Equivalent public locators may share that lifecycle while each split or
multi-channel child keeps its own saved URL. Picker requests are independent
subscriber gates: cancelling one suppresses its callback without cancelling
shared work. Application shutdown first closes those gates and stops lifecycle
work, then saves the retained layout and closes provider channels.

The primary and nested multi-channel pickers resolve asynchronously. While a
request is pending, repeated acceptance is disabled. Errors are sanitized and
retryable; closing a picker cancels its subscriber before widget destruction.

Deterministic coverage is in `tests/src/RumbleApplication.cpp` and is selected
by:

```sh
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='RumbleApplication*:*RumbleChannelPicker*:*RumbleWindowDescriptor*:*RumbleProviderLifetime*'
```
