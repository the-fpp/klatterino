// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/MultiChannelRouting.hpp"

#include <gtest/gtest.h>

#include <array>

namespace chatterino {

TEST(MultiChannelRouting, PrimaryCompatibleAlwaysWins)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 100},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 200},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 0);
}

TEST(MultiChannelRouting, GreatestEmoteSupportOutranksPrimaryAndActivity)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 1,
                                   .activitySequence = 300},
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 3,
                                   .activitySequence = 100},
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 2,
                                   .activitySequence = 500},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 1);
}

TEST(MultiChannelRouting, PrimaryBreaksGreatestSupportTie)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 2,
                                   .activitySequence = 1},
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 2,
                                   .activitySequence = 999},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 0);
}

TEST(MultiChannelRouting, ActivityThenOrderBreakNonPrimarySupportTie)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 1,
                                   .activitySequence = 999},
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 2,
                                   .activitySequence = 50},
        MultiChannelRouteCandidate{.sendable = true,
                                   .supportedEmoteOccurrences = 2,
                                   .activitySequence = 50},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 1);
}

TEST(MultiChannelRouting, FallsBackToMostRecentCompatibleChild)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 300},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 100},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 200},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 2);
}

TEST(MultiChannelRouting, ChildOrderBreaksActivityTie)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 0},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 50},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 50},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 1);
}

TEST(MultiChannelRouting, DisconnectedCandidatesAreIgnored)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 500},
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 400},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 1},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), 2);
}

TEST(MultiChannelRouting, NoCompatibleDestinationReturnsNullopt)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 10},
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 20},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, 0), std::nullopt);
}

TEST(MultiChannelRouting, PrimaryOnlyNeverReroutesChannelSpecificInput)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = false, .activitySequence = 10},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 20},
    };

    EXPECT_EQ(selectMultiChannelDestination(
                  candidates, 0, MultiChannelRoutePolicy::PrimaryOnly),
              std::nullopt);
}

TEST(MultiChannelRouting, PrimaryOnlyStillAllowsPrimaryDestination)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 10},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 20},
    };

    EXPECT_EQ(selectMultiChannelDestination(
                  candidates, 0, MultiChannelRoutePolicy::PrimaryOnly),
              0);
}

TEST(MultiChannelRouting, InvalidPrimaryCanStillUseCompatibleFallback)
{
    const std::array candidates{
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 10},
        MultiChannelRouteCandidate{.sendable = true, .activitySequence = 20},
    };

    EXPECT_EQ(selectMultiChannelDestination(candidates, candidates.size()), 1);
    EXPECT_EQ(selectMultiChannelDestination(
                  candidates, candidates.size(),
                  MultiChannelRoutePolicy::PrimaryOnly),
              std::nullopt);
}

}  // namespace chatterino
