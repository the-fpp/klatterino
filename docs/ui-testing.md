# Local GUI testing with Xephyr

The repository packages a local UI harness that runs Chatterino in a nested,
authenticated X server. Xephyr's single outer window connects to the host
Xwayland display, so a Wayland compositor such as river can move that one
window to a hidden tag. Chatterino itself receives only Xephyr's inner
`DISPLAY`; the harness removes the host Wayland display variables and uses an
isolated profile.

This is for local native smoke testing. It is intentionally not a NixOS VM or
CI check: live provider behavior needs normal network access, and the useful
outer window is meant to be controlled from the local desktop.

## Build once, control quickly

Build both the current vendored Chatterino and the controller:

```sh
nix build .#chatterino-ui-xephyr -o result-ui
```

When changing only the controller, use the lightweight output so Chatterino
does not rebuild:

```sh
nix build .#chatterino-ui-controller -o result-ui-controller
result-ui-controller/bin/chatterino-ui-xephyr --session rumble start \
  --chatterino /path/to/chatterino
```

Start a named session:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble start
```

The command starts authenticated Xephyr with TCP disabled, Openbox, a private
profile, and Chatterino in `--safe-mode`. The profile and artifacts stay under
the printed private session directory. Use `--plugins` only for plugin UI tests.
Use a new session name for a new clean profile.

For an isolated tab-emit/follower test, install the checkout's plugin into the
private profile and select an unused loopback port. This avoids taking over a
normally running follower on its default port:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble-playback start \
  --tab-emit-source ./tab-emit \
  --tab-emit-port 8876
```

`--tab-emit-source` enables plugin support and `tab-emit` only in the isolated
profile. The copied test instance is the only file whose endpoint changes; the
checkout and the user's normal profile/plugin remain untouched.

The nested display defaults to 1920×1080. Chromium-family Rumble login uses a
temporary app-mode window with responsive portrait outer bounds fitted to the
available display (750×1000 on the default test display). It is centered,
movable, and resizable rather than fullscreen; tabs and the address bar stay
absent while the window manager retains its normal frame. The installed
browser keeps its real user agent and platform identity so external identity
providers see an ordinary supported browser. Chatterino launches it first,
then attaches its matching driver over a loopback-only DevTools endpoint; this
avoids WebDriver's automation extension while retaining cookie access after
sign-in. Firefox has no supported app-mode equivalent, so it is the last
installed-browser fallback and uses an isolated, fitted normal window with
ordinary Firefox chrome instead of forcing kiosk/fullscreen. Once the session
cookie appears, the completed Rumble document is hidden while Chatterino
closes the temporary browser and validates the account.

Pass `--qt-scale-factor 2` to a `start` or `*-once` command for a deterministic
2× native rendering pass; omit it for the normal platform scale.

Rumble's Ketch consent prompt remains entirely user-controlled: Chatterino
never clicks it or changes its state. During login Chatterino only listens for
Ketch's experience-hidden event. When the experience is hidden, Chatterino
reads Ketch's final consent-purpose map once. Accept All and Essential Only
both leave Rumble's `essential_services` purpose enabled and continue
normally; an absent or false `essential_services` purpose cancels sign-in.
Rumble's current Ketch configuration also keeps this non-opt-out purpose
enabled after Reject All, while disabling its three optional purposes. Until a
choice is made, Rumble's first-party form and external-provider controls are
visibly disabled and removed from keyboard navigation; Ketch's own roots are
the only interactive page surface. The guard restores the page immediately
after an accepted choice. An external navigation while consent is still
pending remains a fail-closed fallback. Chatterino does not inspect button
labels or infer the choice from event ordering.
The shipped Qt/C++ login path has no X11 tool dependency: Selenium Manager
selects a matching native driver for Firefox, Chrome, Chromium, Edge, or Brave.
The controller opts the nested browser into X11 and software rendering only
inside the test session so root-window screenshots remain deterministic;
normal login launches do not force a display backend or disable the GPU.

Settings, provider data, and Chatterino caches remain isolated per profile.
Only fontconfig's non-sensitive font metadata cache is shared between harness
sessions, avoiding a repeated Qt cold-cache delay for every new clean profile.
The matching browser-driver binary and Selenium Manager metadata are also
shared in the controller's private runtime directory; browser profiles,
cookies, credentials, and Chatterino settings are never shared.
The first session after the runtime directory is recreated can take up to three
minutes while Qt populates that cache; warmed sessions normally start in a few
seconds.

The outer window is named `Chatterino UI Test (SESSION)`. Match that title in a
river rule and assign it to a spare tag using the same rule syntax as the rest
of your river configuration. Do not match the inner Chatterino window: river
only manages Xephyr's outer Xwayland window.

For example, place every harness window on tag 64 and bind a key to hide or
show that tag:

```sh
riverctl rule-add -app-id Xephyr tags 64
riverctl map normal Mod4 Z toggle-focused-tags 64
```

This matches the existing `Xephyr` rule and `Mod4+Z` binding in the local river
configuration. Tag 64 is visible while it is included in `set-focused-tags`;
toggle it off to hide the outer test window.

Xephyr necessarily connects to the host Xwayland display. The inner
application cannot fall back to the host Wayland display because the controller
sets `QT_QPA_PLATFORM=xcb`, replaces `DISPLAY` and `XAUTHORITY`, and removes
`WAYLAND_DISPLAY` and `WAYLAND_SOCKET`. It also uses an Xauthority cookie rather
than the insecure `-ac` option and passes `-nolisten tcp` to Xephyr.

## Fast automation commands

Inspect state without sending a screenshot to a vision model:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble status
result-ui/bin/chatterino-ui-xephyr --session rumble windows
```

`rumble-login-open` waits past Chromium's initial blank app window and returns
only after the public login document has a real title. This removes a common
screenshot/input race while keeping the attended login itself under user
control.

Run the complete channel-picker sequence in one command:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble \
  rumble-open 'https://rumble.com/c/APPROVED_PUBLIC_CHANNEL'
```

For model-driven automation, start an ephemeral clean session, run the whole
flow, and stop it in one call:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble-smoke \
  rumble-smoke-once 'https://rumble.com/c/APPROVED_PUBLIC_CHANNEL'
```

`rumble-smoke-once` installs cleanup traps for normal completion, failure,
interrupt, and termination, so an interrupted automation call does not leave
an outer Xephyr window on the desktop. Use `start` followed by `rumble-smoke`
only when a persistent interactive session is wanted.

The smoke flow saves a `rumble-chat` capture before entering
`/rumble-status`, then saves the status capture separately. This keeps live
message, badge, and username rendering available for visual inspection without
leaving the nested display open.

Run the provider-isolated usercard check without live service data:

```sh
result-ui-controller/bin/chatterino-ui-xephyr \
  --session rumble-usercard rumble-usercard-smoke-once \
  --chatterino /path/to/current/chatterino
```

The controller enables one narrowly scoped test environment hook which injects
a fixed synthetic Rumble message after the application window starts. The
fixture also assigns a contrasting Twitch 7TV paint to the same synthetic
login. The controller confirms the real message view retains the Rumble author
color instead of that foreign paint, clicks the author, then
confirms the card contains the packaged Rumble presentation and no Twitch/Kick
account metadata or controls. OCR is limited to fixed synthetic labels; the
flow contacts no provider and always tears down the nested display.

Run the ordinary-message fallback for an unsupported Rumble reply in the real
composer without provider or account data:

```sh
result-ui-controller/bin/chatterino-ui-xephyr \
  --session rumble-reply rumble-reply-smoke-once \
  --chatterino /path/to/current/chatterino
```

The fixture opens a synthetic Rumble channel with a reply already composed.
The controller presses Enter in the real input and verifies one dispatch of the
exact visible text, an empty input/reply UI after confirmation, and no
unsupported-reply message. The app reports only fixed pass/fail fields and the
nested display is always torn down.

Run the live-state propagation check through the actual tab and composer
widgets without provider data:

```sh
result-ui-controller/bin/chatterino-ui-xephyr \
  --session rumble-lifecycle rumble-lifecycle-smoke-once \
  --chatterino /path/to/current/chatterino
```

The fixture creates a real `RumbleChannel`, drives it through connected,
backoff, confirmed-offline, and replacement-live transitions, and checks the
actual notebook tab after every step. It also verifies that the same split,
public layout identity, display name, and composer draft survive both
directions. The fixture publishes initial, changed, cleared, and replacement
stream titles through the production channel seam, checks the actual split
header after each lifecycle transition, and proves the notebook tab title does
not change. The controller OCRs only the final fixed synthetic result and
always tears down the nested display. Public live-to-ended observation and the
schema-3 visual check remain separate sanitized release-evidence rows.

Run the offline-first automatic-routing transition through the actual split,
composer, destination badge, and send path without provider data:

```sh
result-ui-controller/bin/chatterino-ui-xephyr \
  --session automatic-routing automatic-routing-smoke-once \
  --chatterino /path/to/current/chatterino
```

The fixture keeps a synthetic Rumble child first in the persisted layout while
it is confirmed offline and verifies that an offline-but-writable Twitch child
is previewed and receives exactly one UI-entered send. It then makes the same
Rumble child eligible, checks that automatic preview returns to the documented
primary, and enters a second exactly-once send. Both drafts and child order are
checked in the running application. The fixture then returns to the offline
fallback so the inline automatic-routing glyph is visible beside the Twitch
badge, and verifies that the old successful-routing chat notice is absent. Run
once normally and once with `--qt-scale-factor 2`; the controller reports only
fixed result fields and always tears down the nested display.

Run the compact revealed-tab interaction through the actual notebook, add-tab
button, empty tab, and channel picker:

```sh
result-ui-controller/bin/chatterino-ui-xephyr \
  --session compact-tab-reveal compact-tab-reveal-smoke-once \
  --chatterino /path/to/current/chatterino
```

The fixture reveals the normal tab strip in compact mode, selects a
pre-existing empty tab, returns to the original tab, and clicks the real `+`
button. It verifies that exactly one distinct tab is selected, that its channel
picker opens and closes while the reveal remains active, and that a later
outside click dismisses the reveal. Run once normally and once with
`--qt-scale-factor 2`; the controller reports only fixed result fields and
always tears down the nested display.

Run the complete temporary-browser login cancellation and cleanup check in one
command:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble-login-smoke \
  rumble-login-smoke-once
```

This opens Accounts, prepares a matching driver, launches a real installed
browser against Rumble, confirms its outer window fits the nested display and
can be moved and resized, cancels through Chatterino, confirms the browser and
driver stopped,
and tears down the nested display. It reports only fixed result fields and
local artifact paths. There are no separate host `xdotool`, process-inspection,
or cleanup commands to approve. `--selenium-manager PATH`,
`--webdriver-loader PATH`, and `--webdriver-library-path PATHS` support raw
local/Nix builds. `--rumble-login-helper PATH` remains a deterministic
test-only protocol override.

For a user-attended live login check, run:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble-login-live \
  rumble-login-live-once
```

The command waits up to ten minutes for the user to complete Rumble's own login
UI. It reports only whether Chatterino added the resulting account, never the
credential or OCR text, and always tears down the temporary browser and
display.

The controller uses deterministic keystrokes, window IDs, and short polling
intervals. It OCRs only the fixed, safe status fields and does not print raw
screen text. This removes model round trips for individual clicks.

When visual inspection is needed, save a full local PNG plus a stripped WebP
preview bounded to 800 pixels:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble shot checkpoint main
```

Use the WebP for model inspection. Keep the full PNG local for debugging; a
Rumble screen can contain channel or message identities and is not an issue
attachment.

For manual tools, print the inner environment:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble env
```

Stop the processes while preserving the isolated profile and artifacts:

```sh
result-ui/bin/chatterino-ui-xephyr --session rumble stop
```

Starting the same session again reuses its profile, which is useful for layout
restore checks. Choose a different session name when the test requires a clean
profile.
