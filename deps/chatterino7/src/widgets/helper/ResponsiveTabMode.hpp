// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace chatterino {

/// Geometry snapshot used to calculate responsive compact-tab state.
///
/// windowHeight is the usable client height. availableScreenHeight excludes
/// operating-system reserved areas such as task bars and menu bars.
struct ResponsiveTabGeometry {
    int windowHeight = 0;
    int availableScreenHeight = 0;
    bool maximized = false;
    bool fullscreen = false;
};

/// Returns whether a window should expose compact tab navigation.
///
/// Normal windows enter at or below half of the containing display's
/// available height. Once active, a two-percentage-point exit margin prevents
/// flapping during interactive resize and tiling. Maximized and fullscreen
/// windows always use normal tabs.
constexpr bool shouldUseCompactTabs(const ResponsiveTabGeometry &geometry,
                                    bool currentlyCompact)
{
    if (geometry.windowHeight <= 0 || geometry.availableScreenHeight <= 0 ||
        geometry.maximized || geometry.fullscreen)
    {
        return false;
    }

    constexpr std::int64_t PERCENT_SCALE = 100;
    constexpr std::int64_t ENTER_PERCENT = 50;
    constexpr std::int64_t EXIT_PERCENT = 52;
    const auto windowHeight = static_cast<std::int64_t>(geometry.windowHeight);
    const auto screenHeight =
        static_cast<std::int64_t>(geometry.availableScreenHeight);
    const auto limit = currentlyCompact ? EXIT_PERCENT : ENTER_PERCENT;
    return windowHeight * PERCENT_SCALE <= screenHeight * limit;
}

}  // namespace chatterino
