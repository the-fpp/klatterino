// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace chatterino {

struct CompactTabEntry {
    bool navigable = true;
    bool live = false;
};

struct CompactTabStatus {
    /// One-based among navigable tabs, or zero when no navigable tab is
    /// selected.
    size_t selectedPosition = 0;
    size_t navigableCount = 0;
    size_t liveNavigableCount = 0;
    bool controlsEnabled = false;
    bool liveControlsEnabled = false;
};

/// Computes the one-based selected position plus normal and live-only counts
/// for compact tab controls. Filtered/hidden tabs are excluded from navigation
/// and both counts.
constexpr CompactTabStatus compactTabStatus(
    std::span<const CompactTabEntry> tabs,
    std::optional<size_t> selectedIndex) noexcept
{
    CompactTabStatus status;
    for (size_t index = 0; index < tabs.size(); ++index)
    {
        if (!tabs[index].navigable)
        {
            continue;
        }
        ++status.navigableCount;
        if (tabs[index].live)
        {
            ++status.liveNavigableCount;
        }
        if (selectedIndex && *selectedIndex == index)
        {
            status.selectedPosition = status.navigableCount;
        }
    }
    status.controlsEnabled = status.navigableCount > 1;
    status.liveControlsEnabled = status.liveNavigableCount > 1;
    return status;
}

}  // namespace chatterino
