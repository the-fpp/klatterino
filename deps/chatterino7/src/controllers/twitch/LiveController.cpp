// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/twitch/LiveController.hpp"

#include "common/QLogging.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"

#include <QDebug>

#include <utility>
#include <vector>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const auto &LOG = chatterinoTwitchLiveController;

}  // namespace

namespace chatterino {

TwitchLiveController::TwitchLiveController()
{
    QObject::connect(&this->refreshTimer, &QTimer::timeout, [this] {
        this->request();
    });
    this->refreshTimer.start(TwitchLiveController::REFRESH_INTERVAL);

    QObject::connect(&this->immediateRequestTimer, &QTimer::timeout, [this] {
        QStringList channelIDs;

        {
            std::unique_lock immediateRequestsLock(
                this->immediateRequestsMutex);
            for (const auto &channelID : this->immediateRequests)
            {
                channelIDs.append(channelID);
            }
            this->immediateRequests.clear();
        }

        if (channelIDs.isEmpty())
        {
            return;
        }

        this->request(channelIDs);
    });
    this->immediateRequestTimer.start(
        TwitchLiveController::IMMEDIATE_REQUEST_INTERVAL);

    this->liveRefreshTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&this->liveRefreshTimer, &QTimer::timeout, [this] {
        this->requestLiveChannels();
    });
    this->liveRefreshTimer.start(
        TwitchLiveController::LIVE_REFRESH_INTERVAL);
}

void TwitchLiveController::add(const std::shared_ptr<TwitchChannel> &newChannel)
{
    assert(newChannel != nullptr);

    const auto channelID = newChannel->roomId();
    assert(!channelID.isEmpty());

    {
        std::unique_lock lock(this->channelsMutex);
        auto &entry = this->channels[channelID];
        entry.ptr = newChannel;
        entry.wasChecked = false;
        ++entry.statusGeneration;
    }

    {
        std::unique_lock immediateRequestsLock(this->immediateRequestsMutex);
        this->immediateRequests.emplace(channelID);
    }
}

void TwitchLiveController::requestImmediateRefresh(const QString &channelID)
{
    runInGuiThread([this, channelID] {
        this->requestStreamStatuses(QStringList{channelID});
    });
}

void TwitchLiveController::markChannelOffline(const QString &channelID)
{
    runInGuiThread([this, channelID] {
        std::shared_ptr<TwitchChannel> channel;

        {
            std::unique_lock lock(this->channelsMutex);
            auto it = this->channels.find(channelID);
            if (it == this->channels.end())
            {
                return;
            }

            ++it->second.statusGeneration;
            it->second.wasChecked = true;
            channel = it->second.ptr.lock();
            if (!channel)
            {
                this->channels.erase(it);
                return;
            }
        }

        channel->updateStreamStatus(std::nullopt, false);
    });
}

void TwitchLiveController::request(std::optional<QStringList> optChannelIDs)
{
    QStringList channelIDs;

    {
        std::shared_lock lock(this->channelsMutex);

        if (optChannelIDs)
        {
            for (const auto &channelID : *optChannelIDs)
            {
                if (this->channels.contains(channelID))
                {
                    channelIDs.append(channelID);
                }
            }
        }
        else
        {
            for (const auto &[channelID, entry] : this->channels)
            {
                (void)entry;
                channelIDs.append(channelID);
            }
        }
    }

    if (channelIDs.isEmpty())
    {
        return;
    }

    this->requestStreamStatuses(channelIDs);
    this->requestChannelMetadata(channelIDs);
}

void TwitchLiveController::requestStreamStatuses(
    const QStringList &requestedChannelIDs, std::function<void()> finally)
{
    QStringList channelIDs;
    std::unordered_map<QString, std::uint64_t> statusGenerations;

    {
        std::unique_lock lock(this->channelsMutex);
        for (const auto &channelID : requestedChannelIDs)
        {
            auto it = this->channels.find(channelID);
            if (it == this->channels.end())
            {
                continue;
            }

            channelIDs.append(channelID);
            statusGenerations.emplace(channelID,
                                      ++it->second.statusGeneration);
        }
    }

    if (channelIDs.isEmpty())
    {
        if (finally)
        {
            finally();
        }
        return;
    }

    auto batches =
        splitListIntoBatches(channelIDs, TwitchLiveController::BATCH_SIZE);
    auto remainingBatches =
        std::make_shared<std::atomic_size_t>(batches.size());

    qCDebug(LOG) << "Make" << batches.size() << "stream status requests";

    for (const auto &batch : batches)
    {
        std::unordered_map<QString, std::uint64_t> batchGenerations;
        for (const auto &channelID : batch)
        {
            batchGenerations.emplace(channelID,
                                     statusGenerations.at(channelID));
        }

        getHelix()->fetchStreams(
            batch, {},
            [this, batch{batch},
             batchGenerations{std::move(batchGenerations)}](
                const auto &streams) {
                std::unordered_map<QString, std::optional<HelixStream>> results;

                for (const auto &channelID : batch)
                {
                    results[channelID] = std::nullopt;
                }

                for (const auto &stream : streams)
                {
                    results[stream.userId] = stream;
                }

                struct PendingUpdate {
                    std::shared_ptr<TwitchChannel> channel;
                    std::optional<HelixStream> stream;
                    bool isInitialUpdate;
                };

                std::vector<PendingUpdate> updates;
                QStringList deadChannels;

                {
                    std::unique_lock lock(this->channelsMutex);
                    for (const auto &[channelID, stream] : results)
                    {
                        auto it = this->channels.find(channelID);
                        if (it == this->channels.end())
                        {
                            continue;
                        }

                        const auto generation =
                            batchGenerations.find(channelID);
                        if (generation == batchGenerations.end() ||
                            generation->second != it->second.statusGeneration)
                        {
                            continue;
                        }

                        if (auto channel = it->second.ptr.lock(); channel)
                        {
                            updates.push_back({
                                .channel = std::move(channel),
                                .stream = stream,
                                .isInitialUpdate = !it->second.wasChecked,
                            });
                            it->second.wasChecked = true;
                        }
                        else
                        {
                            deadChannels.append(channelID);
                        }
                    }

                    for (const auto &deadChannel : deadChannels)
                    {
                        this->channels.erase(deadChannel);
                    }
                }

                for (const auto &update : updates)
                {
                    update.channel->updateStreamStatus(
                        update.stream, update.isInitialUpdate);
                }
            },
            [] {
                qCWarning(LOG) << "Failed stream check request";
            },
            [remainingBatches, finally] {
                if (remainingBatches->fetch_sub(1) == 1 && finally)
                {
                    finally();
                }
            });
    }
}

void TwitchLiveController::requestChannelMetadata(
    const QStringList &channelIDs)
{
    auto batches =
        splitListIntoBatches(channelIDs, TwitchLiveController::BATCH_SIZE);

    for (const auto &batch : batches)
    {
        getHelix()->fetchChannels(
            batch,
            [this](const auto &helixChannels) {
                QStringList deadChannels;

                {
                    std::shared_lock lock(this->channelsMutex);
                    for (const auto &helixChannel : helixChannels)
                    {
                        auto it = this->channels.find(helixChannel.userId);
                        if (it == this->channels.end())
                        {
                            continue;
                        }

                        if (auto channel = it->second.ptr.lock(); channel)
                        {
                            channel->updateStreamTitle(helixChannel.title);
                            channel->updateDisplayName(helixChannel.name);
                        }
                        else
                        {
                            deadChannels.append(helixChannel.userId);
                        }
                    }
                }

                if (!deadChannels.isEmpty())
                {
                    std::unique_lock lock(this->channelsMutex);
                    for (const auto &deadChannel : deadChannels)
                    {
                        this->channels.erase(deadChannel);
                    }
                }
            },
            [] {
                qCWarning(LOG) << "Failed channel metadata request";
            });
    }
}

void TwitchLiveController::requestLiveChannels()
{
    if (this->liveRefreshInFlight.exchange(true))
    {
        return;
    }

    QStringList liveChannelIDs;

    {
        std::shared_lock lock(this->channelsMutex);
        for (const auto &[channelID, entry] : this->channels)
        {
            if (auto channel = entry.ptr.lock(); channel && channel->isLive())
            {
                liveChannelIDs.append(channelID);
            }
        }
    }

    if (liveChannelIDs.isEmpty())
    {
        this->liveRefreshInFlight.store(false);
        return;
    }

    this->requestStreamStatuses(liveChannelIDs, [this] {
        this->liveRefreshInFlight.store(false);
    });
}

}  // namespace chatterino
