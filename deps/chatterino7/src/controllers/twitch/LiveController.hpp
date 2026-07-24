// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QStringHash.hpp"

#include <QString>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace chatterino {

class TwitchChannel;

class ITwitchLiveController
{
public:
    virtual ~ITwitchLiveController() = default;

    virtual void add(const std::shared_ptr<TwitchChannel> &newChannel) = 0;

    /// Invalidates older in-flight status requests and immediately refreshes
    /// this channel through Helix.
    virtual void requestImmediateRefresh(const QString &channelID)
    {
        (void)channelID;
    }

    /// Invalidates older in-flight status requests and marks this channel
    /// offline immediately.
    virtual void markChannelOffline(const QString &channelID)
    {
        (void)channelID;
    }
};

class TwitchLiveController : public ITwitchLiveController
{
public:
    // Controls how often all channels have their stream status refreshed.
    static constexpr std::chrono::seconds REFRESH_INTERVAL{30};

    // Controls how quickly new channels have their stream status loaded.
    static constexpr std::chrono::milliseconds IMMEDIATE_REQUEST_INTERVAL{250};

    // Controls how often channels believed to be live are checked for ending.
    static constexpr std::chrono::milliseconds LIVE_REFRESH_INTERVAL{250};

    /**
     * How many channels to include in a single request
     *
     * Should not be more than 100
     **/
    static constexpr int BATCH_SIZE{100};

    TwitchLiveController();

    // Add a Twitch channel to be queried for live status.
    void add(const std::shared_ptr<TwitchChannel> &newChannel) override;

    void requestImmediateRefresh(const QString &channelID) override;
    void markChannelOffline(const QString &channelID) override;

private:
    struct ChannelEntry {
        std::weak_ptr<TwitchChannel> ptr;
        bool wasChecked = false;

        // Incremented whenever a stream-status request is started or an
        // EventSub status update is applied. Responses from an older generation
        // are ignored so they cannot restore stale state.
        std::uint64_t statusGeneration = 0;
    };

    /**
     * Run batched Helix Channels & Stream requests for channels.
     *
     * If a list of channel IDs is passed to request, only those channels are
     * requested. Otherwise all known channels are requested.
     **/
    void request(std::optional<QStringList> optChannelIDs = std::nullopt);

    /// Refresh only stream state for the supplied channel IDs.
    void requestStreamStatuses(const QStringList &channelIDs,
                               std::function<void()> finally = {});

    /// Refresh title and display-name metadata for the supplied channel IDs.
    void requestChannelMetadata(const QStringList &channelIDs);

    /// Fast path used only while at least one tracked channel is live.
    void requestLiveChannels();

    /**
     * List of channel IDs pointing to their Twitch Channel.
     **/
    std::unordered_map<QString, ChannelEntry> channels;
    std::shared_mutex channelsMutex;

    /**
     * List of channels that need an initial live status update.
     **/
    std::unordered_set<QString> immediateRequests;
    std::mutex immediateRequestsMutex;

    /**
     * Timer responsible for refreshing all channels.
     **/
    QTimer refreshTimer;

    /**
     * Timer responsible for refreshing newly added channels.
     **/
    QTimer immediateRequestTimer;

    /**
     * Timer responsible for rapidly checking channels currently believed live.
     **/
    QTimer liveRefreshTimer;
    std::atomic_bool liveRefreshInFlight{false};
};

}  // namespace chatterino
