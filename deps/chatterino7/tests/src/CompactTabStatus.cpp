// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/CompactTabStatus.hpp"

#include <gtest/gtest.h>

#include <array>

namespace chatterino {

TEST(CompactTabStatus, ExcludesFilteredTabsFromCountAndPosition)
{
    const std::array tabs{
        CompactTabEntry{.navigable = true, .live = true},
        CompactTabEntry{.navigable = false, .live = true},
        CompactTabEntry{.navigable = true, .live = false},
    };

    const auto status = compactTabStatus(tabs, 2);
    EXPECT_EQ(status.selectedPosition, 2);
    EXPECT_EQ(status.navigableCount, 2);
    EXPECT_TRUE(status.controlsEnabled);
    EXPECT_EQ(status.liveNavigableCount, 1);
    EXPECT_FALSE(status.liveControlsEnabled);
}

TEST(CompactTabStatus, LiveControlsRequireTwoNavigableLiveTabs)
{
    const std::array tabs{
        CompactTabEntry{.navigable = true, .live = true},
        CompactTabEntry{.navigable = false, .live = true},
        CompactTabEntry{.navigable = true, .live = true},
        CompactTabEntry{.navigable = true, .live = false},
    };

    const auto status = compactTabStatus(tabs, 0);
    EXPECT_EQ(status.navigableCount, 3);
    EXPECT_EQ(status.liveNavigableCount, 2);
    EXPECT_TRUE(status.controlsEnabled);
    EXPECT_TRUE(status.liveControlsEnabled);
}

TEST(CompactTabStatus, ZeroAndOneTabDisableControls)
{
    const std::array<CompactTabEntry, 0> empty{};
    EXPECT_EQ(compactTabStatus(empty, std::nullopt).navigableCount, 0);
    EXPECT_FALSE(compactTabStatus(empty, std::nullopt).controlsEnabled);

    const std::array one{CompactTabEntry{.navigable = true}};
    const auto status = compactTabStatus(one, 0);
    EXPECT_EQ(status.selectedPosition, 1);
    EXPECT_EQ(status.navigableCount, 1);
    EXPECT_FALSE(status.controlsEnabled);
}

TEST(CompactTabStatus, MissingOrFilteredSelectionHasZeroPosition)
{
    const std::array tabs{
        CompactTabEntry{.navigable = false},
        CompactTabEntry{.navigable = true},
        CompactTabEntry{.navigable = true},
    };

    const auto missing = compactTabStatus(tabs, std::nullopt);
    EXPECT_EQ(missing.selectedPosition, 0);
    EXPECT_EQ(missing.navigableCount, 2);
    EXPECT_TRUE(missing.controlsEnabled);

    const auto filtered = compactTabStatus(tabs, 0);
    EXPECT_EQ(filtered.selectedPosition, 0);
    EXPECT_EQ(filtered.navigableCount, 2);
    EXPECT_TRUE(filtered.controlsEnabled);
}

}  // namespace chatterino
