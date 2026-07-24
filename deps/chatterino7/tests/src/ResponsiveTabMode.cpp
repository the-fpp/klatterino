// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ResponsiveTabMode.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace chatterino;

namespace {

ResponsiveTabGeometry geometry(int windowHeight, int screenHeight = 1000)
{
    return {
        .windowHeight = windowHeight,
        .availableScreenHeight = screenHeight,
    };
}

}  // namespace

TEST(ResponsiveTabMode, EntersAtHalfAvailableScreenHeight)
{
    EXPECT_TRUE(shouldUseCompactTabs(geometry(499), false));
    EXPECT_TRUE(shouldUseCompactTabs(geometry(500), false));
    EXPECT_FALSE(shouldUseCompactTabs(geometry(501), false));
}

TEST(ResponsiveTabMode, UsesExitHysteresisWhileCompact)
{
    EXPECT_TRUE(shouldUseCompactTabs(geometry(501), true));
    EXPECT_TRUE(shouldUseCompactTabs(geometry(520), true));
    EXPECT_FALSE(shouldUseCompactTabs(geometry(521), true));

    // Once inactive, a window in the hysteresis band stays inactive.
    EXPECT_FALSE(shouldUseCompactTabs(geometry(510), false));
}

TEST(ResponsiveTabMode, MaximizedAndFullscreenWindowsUseNormalTabs)
{
    auto maximized = geometry(400);
    maximized.maximized = true;
    EXPECT_FALSE(shouldUseCompactTabs(maximized, false));
    EXPECT_FALSE(shouldUseCompactTabs(maximized, true));

    auto fullscreen = geometry(400);
    fullscreen.fullscreen = true;
    EXPECT_FALSE(shouldUseCompactTabs(fullscreen, false));
    EXPECT_FALSE(shouldUseCompactTabs(fullscreen, true));
}

TEST(ResponsiveTabMode, RecomputesAgainstChangedContainingScreen)
{
    const auto windowHeight = 500;
    EXPECT_TRUE(shouldUseCompactTabs(geometry(windowHeight, 1000), false));
    EXPECT_FALSE(shouldUseCompactTabs(geometry(windowHeight, 900), true));
    EXPECT_TRUE(shouldUseCompactTabs(geometry(windowHeight, 1200), false));
}

TEST(ResponsiveTabMode, InvalidGeometryFailsToNormalTabs)
{
    EXPECT_FALSE(shouldUseCompactTabs(geometry(0), true));
    EXPECT_FALSE(shouldUseCompactTabs(geometry(500, 0), true));
    EXPECT_FALSE(shouldUseCompactTabs(geometry(-1), false));
}

TEST(ResponsiveTabMode, GeometryEventSequenceDoesNotFlap)
{
    auto maximized = geometry(400);
    maximized.maximized = true;
    auto fullscreen = geometry(400);
    fullscreen.fullscreen = true;

    struct Step {
        const char *event;
        ResponsiveTabGeometry geometry;
        bool expectedCompact;
    };
    const std::array steps{
        Step{"resize enters at 50 percent", geometry(500), true},
        Step{"resize stays compact in hysteresis", geometry(510), true},
        Step{"resize stays compact at exit boundary", geometry(520), true},
        Step{"resize exits above 52 percent", geometry(521), false},
        Step{"resize stays normal in hysteresis", geometry(510), false},
        Step{"restore recomputes compact", geometry(400), true},
        Step{"maximize forces normal", maximized, false},
        Step{"restore after maximize recomputes", geometry(400), true},
        Step{"screen association uses shorter display",
             geometry(500, 900), false},
        Step{"available geometry or DPI change uses taller display",
             geometry(500, 1200), true},
        Step{"fullscreen forces normal", fullscreen, false},
        Step{"restore after fullscreen recomputes", geometry(500), true},
    };

    auto compact = false;
    for (const auto &step : steps)
    {
        SCOPED_TRACE(step.event);
        compact = shouldUseCompactTabs(step.geometry, compact);
        EXPECT_EQ(compact, step.expectedCompact);
    }
}

