# Compact tab controls validation

Responsive compact mode is presentation-only. It preserves the selected page,
tab order, persisted tab-visibility preference, and live-only filter. Its count
and traversal use the same wrapped visibility filter as `Notebook` navigation,
and its buttons delegate to `selectPreviousTab()` and `selectNextTab()`.

`CompactTabControls.cpp` provides deterministic widget coverage for:

- compact enter/exit and user-visibility composition;
- full notebook-content geometry when controls live in the custom titlebar;
- zero, one, and many navigable-tab states;
- add, remove, reorder, rename, selection, and live-only filter updates;
- forward/backward wraparound through the notebook APIs;
- focused page-input restoration and non-focusable buttons;
- tooltip and accessible name/description content;
- non-overlapping, in-bounds geometry at top, bottom, left, and right tab
  locations, at 1x and 2x UI scale;
- dark/light theme palette contrast and state retention.

After building the test target, the focused output can be captured with:

```shell
QT_QPA_PLATFORM=minimal ./build/bin/chatterino-test \
  --gtest_filter='CompactTabControlsTest.*:CompactTabStatus.*'
```

Adjust `./build` if the configured build directory has another name.

The deterministic tests are the required automated geometry and behavior gate.
A headless Linux test job cannot capture the Windows `USEWINSDK`
custom-titlebar chrome. The remaining external visual check is native Windows
titlebar composition next to the main-window account label, plus placement in
popup/attached custom frames, at 1x and 2x scale in both light and dark themes.
That path uses the same `initializeCompactTabControlButtons` and
`applyCompactTabControlState` helpers as the tested notebook-hosted controls;
only the operating-system chrome is platform-specific. Record that check in
issue #28 using its versioned result template; it is not claimed by headless CI.
