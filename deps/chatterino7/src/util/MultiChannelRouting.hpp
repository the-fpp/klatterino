// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace chatterino {

/// Side-effect-free routing input for one child channel.
///
/// activitySequence is a process-local monotonic sequence assigned when a live
/// chat message is appended. It intentionally does not use wall-clock time, so
/// reconnects and clock adjustments cannot reorder already observed activity.
struct MultiChannelRouteCandidate {
    bool sendable = false;
    size_t supportedEmoteOccurrences = 0;
    uint64_t activitySequence = 0;
};

/// Routing policy for one dispatch attempt.
///
/// Channel-specific commands and replies use PrimaryOnly so their meaning is
/// never silently changed by routing to a different child. Ordinary messages
/// use CompatibleFallback.
enum class MultiChannelRoutePolicy {
    CompatibleFallback,
    PrimaryOnly,
};

/// Selects exactly one destination from an immutable compatibility snapshot.
/// With CompatibleFallback, the destination supporting the most emote
/// occurrences wins. The primary wins a compatibility tie, then greatest
/// activity and child order break remaining ties. PrimaryOnly never reroutes.
/// Returns nullopt when no allowed candidate is sendable.
inline std::optional<size_t> selectMultiChannelDestination(
    std::span<const MultiChannelRouteCandidate> candidates, size_t primaryIndex,
    MultiChannelRoutePolicy policy =
        MultiChannelRoutePolicy::CompatibleFallback) noexcept
{
    if (policy == MultiChannelRoutePolicy::PrimaryOnly)
    {
        if (primaryIndex < candidates.size() &&
            candidates[primaryIndex].sendable)
        {
            return primaryIndex;
        }
        return std::nullopt;
    }

    size_t greatestSupport = 0;
    bool hasSendable = false;
    for (const auto &candidate : candidates)
    {
        if (candidate.sendable)
        {
            greatestSupport =
                std::max(greatestSupport, candidate.supportedEmoteOccurrences);
            hasSendable = true;
        }
    }
    if (!hasSendable)
    {
        return std::nullopt;
    }
    if (primaryIndex < candidates.size() && candidates[primaryIndex].sendable &&
        candidates[primaryIndex].supportedEmoteOccurrences == greatestSupport)
    {
        return primaryIndex;
    }

    std::optional<size_t> selected;
    uint64_t selectedActivity = 0;
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const auto &candidate = candidates[index];
        if (!candidate.sendable ||
            candidate.supportedEmoteOccurrences != greatestSupport)
        {
            continue;
        }
        if (!selected || candidate.activitySequence > selectedActivity)
        {
            selected = index;
            selectedActivity = candidate.activitySequence;
        }
    }
    return selected;
}

}  // namespace chatterino
