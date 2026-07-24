# Responsive-tab regression validation

Issue #29 protects the combined compact-mode policy and Notebook controls added
by #27 and #28. Automated tests remain offline and headless. Native display and
compositor delivery is recorded separately because a synthetic Qt screen cannot
faithfully reproduce every supported desktop.

## Behavior matrix

| Contract | Automated coverage | Native-only coverage |
|---|---|---|
| enter at 50%, exit above 52%, and hysteresis | `ResponsiveTabMode.*`, `ResponsiveTabMode.GeometryEventSequenceDoesNotFlap` | tile/resize across both thresholds |
| maximize/fullscreen/restore | policy sequence test | real window-state delivery on each display |
| resize, containing-screen, available-geometry, and logical-DPI recomputation | table-driven geometry snapshots | compositor screen reassociation and per-monitor scale transition |
| zero/one/many, filtered traversal, wrap, and no selection | `CompactTabControlsTest.*` | visual control order |
| normal/compact transitions and hide-all/live-only preferences | `ResponsiveTabsRegressionTest.*` | installed-profile confirmation |
| temporary tab reveal, direct selection, outside-click dismissal, focus restoration, and embedded/external control layouts | `CompactTabControlsTest.StatusRevealsTabsAndOutsideClickRestoresCompactControls`, `CompactTabControlsTest.DirectSelectionDismissesRevealAndRestoresDestinationFocus`, `CompactTabControlsTest.RevealIsTemporaryAcrossModeAndUserVisibilityTransitions`, `CompactTabControlsTest.ExternalCompactControlLayoutUsesTheSameRevealAndDismissState` | click the compact title, select a tab, then repeat at every supported tab location and scale |
| add/remove/reorder/rename/select/live-filter mutation | compact widget and regression suites | none beyond visual smoke |
| selection and split-input focus | compact widget and regression suites | native focus after titlebar clicks |
| accessibility, tab locations, 1×/2× scale, and light/dark palette | `CompactTabControlsTest.AccessibleGeometrySurvivesLocationsAndScale` | native titlebar/compositor rendering |

## Automated command

After configuring and building the repository test target:

```sh
QT_QPA_PLATFORM=minimal ./build-test/bin/chatterino-test \
  --gtest_filter='ResponsiveTabMode.*:CompactTabStatus.*:CompactTabControlsTest.*:ResponsiveTabsRegressionTest.*'
```

While responsive compact mode is active, the central status text is a
no-focus, accessible **Show tabs** action. It temporarily replaces the compact
controls with the normal tab strip without changing the responsive mode or the
saved show/hide preference. Selecting a tab dismisses the strip after the
selection event; any press elsewhere dismisses it before the target handles
the click. Leaving compact mode also clears the temporary state.

The exact PR head must also pass both repository GitHub Actions jobs.

## Native matrix

Use a sanitized profile built from the merged commit. Move a restored window
between displays with different available heights and scale factors; cross the
50%/52% thresholds; maximize, fullscreen, and restore; then repeat for every
supported tab location. Verify the compact status, control order, selected page,
focus, and current visibility filter.

Do not include account names, channel names, messages, monitor serial numbers,
or identifying screenshots. Paste only:

```text
<!-- compact-tab-regression-native-validation schema=1 -->
COMPACT_TAB_REGRESSION_NATIVE_V1=PASS
tested_commit=<full merged SHA>
platform=<os-and-version>
qt_version=<major.minor.patch>
display_topology=<count; sanitized resolutions and scale percentages>
thresholds_and_hysteresis=PASS
move_and_screen_change=PASS
maximize_fullscreen_restore=PASS
tab_locations=PASS
focus_selection_filter=PASS
notes=none
```

Use `FAIL` and a short non-sensitive note if a row fails. Native evidence
closes #29 but does not block merging a reviewed implementation with green CI.
