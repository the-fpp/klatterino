// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT
#include "providers/rumble/RumbleChannelProvider.hpp"

#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"

#include <QUuid>

#include <cassert>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chatterino {
namespace {
RumbleProviderError invalidKey(RumbleChannelKeyError error)
{
    return {.code = RumbleProviderErrorCode::InvalidKey, .keyError = error};
}
RumbleProviderError providerError(RumbleProviderErrorCode code)
{
    return {.code = code, .keyError = std::nullopt};
}
QString makeChannelIdentity()
{
    auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    uuid.remove(u'-');
    return QStringLiteral("rumble-%1").arg(uuid);
}
}  // namespace
RumbleChannelProvider::RumbleChannelProvider(
    std::shared_ptr<RumbleDispatcher> dispatcher)
    : dispatcher_(std::move(dispatcher))
    , identity_(std::make_shared<const int>(0))
{
    assert(this->dispatcher_ != nullptr);
}
RumbleChannelProvider::~RumbleChannelProvider()
{
    this->shutdown();
}
Expected<std::shared_ptr<RumbleChannel>, RumbleProviderError>
    RumbleChannelProvider::getOrCreate(RumbleChannelKeyKind kind, QString value)
{
    auto key = RumbleChannelKey::normalize(kind, std::move(value));
    if (!key)
    {
        return makeUnexpected(invalidKey(key.error()));
    }
    return this->getOrCreate(*key);
}
Expected<std::shared_ptr<RumbleChannel>, RumbleProviderError>
    RumbleChannelProvider::getOrCreate(const RumbleChannelKey &key)
{
    if (!this->dispatcher_->isOwnerThread())
    {
        return makeUnexpected(
            providerError(RumbleProviderErrorCode::WrongThread));
    }
    std::lock_guard lock(this->mutex_);
    if (this->shutdown_)
    {
        return makeUnexpected(providerError(RumbleProviderErrorCode::Shutdown));
    }
    auto found = this->cache_.find(key);
    if (found != this->cache_.end())
    {
        auto channel = found->second.lock();
        if (channel && !channel->isClosingOrClosed())
        {
            return channel;
        }
        this->cache_.erase(found);
    }
    // This identity is permanent for the channel object but intentionally
    // independent of every provisional or canonical Rumble locator. Its
    // restricted character set is also safe as a log-path component.
    const auto channelIdentity = makeChannelIdentity();
    const auto dispatcher = this->dispatcher_;
    auto channel = std::shared_ptr<RumbleChannel>(
        new RumbleChannel(channelIdentity, key, dispatcher,
                          std::weak_ptr<const void>(this->identity_)),
        [dispatcher](RumbleChannel *channel) noexcept {
            channel->gateForDeferredDestruction();
            // Break the only potential cleanup cycle before the dispatcher
            // takes ownership of the raw channel. All later destruction runs
            // through dispose() on the receiver's affinity thread.
            channel->dispatcher_.reset();
            auto owned = std::shared_ptr<RumbleChannel>(channel);
            dispatcher->dispose([owned = std::move(owned)] {});
        });
    this->cache_.emplace(key, channel);
    return channel;
}
Expected<void, RumbleProviderError> RumbleChannelProvider::associateAlias(
    const std::shared_ptr<RumbleChannel> &channel, RumbleChannelKeyKind kind,
    QString value)
{
    auto key = RumbleChannelKey::normalize(kind, std::move(value));
    if (!key)
    {
        return makeUnexpected(invalidKey(key.error()));
    }
    return this->associateAlias(channel, *key);
}
Expected<void, RumbleProviderError> RumbleChannelProvider::associateAlias(
    const std::shared_ptr<RumbleChannel> &channel,
    const RumbleChannelKey &alias)
{
    if (!this->dispatcher_->isOwnerThread())
    {
        return makeUnexpected(
            providerError(RumbleProviderErrorCode::WrongThread));
    }
    if (!channel || !channel->belongsToProvider(this->identity_))
    {
        return makeUnexpected(
            providerError(RumbleProviderErrorCode::ForeignChannel));
    }
    if (channel->isClosingOrClosed())
    {
        return makeUnexpected(
            providerError(RumbleProviderErrorCode::ClosedChannel));
    }
    std::lock_guard lock(this->mutex_);
    if (this->shutdown_)
    {
        return makeUnexpected(providerError(RumbleProviderErrorCode::Shutdown));
    }
    auto found = this->cache_.find(alias);
    if (found != this->cache_.end())
    {
        auto existing = found->second.lock();
        if (!existing || existing->isClosingOrClosed())
        {
            this->cache_.erase(found);
        }
        else if (existing != channel)
        {
            return makeUnexpected(
                providerError(RumbleProviderErrorCode::AliasConflict));
        }
        else
        {
            return {};
        }
    }
    this->cache_.insert_or_assign(alias, channel);
    return {};
}
void RumbleChannelProvider::shutdown()
{
    std::vector<std::shared_ptr<RumbleChannel>> channels;
    {
        std::lock_guard lock(this->mutex_);
        if (this->shutdown_)
        {
            return;
        }
        this->shutdown_ = true;
        std::unordered_set<RumbleChannel *> seen;
        for (auto &entry : this->cache_)
        {
            auto channel = entry.second.lock();
            if (channel && seen.emplace(channel.get()).second)
            {
                channels.emplace_back(std::move(channel));
            }
        }
        this->cache_.clear();
    }
    for (const auto &channel : channels)
    {
        channel->close();
    }
}
bool RumbleChannelProvider::isShutdown() const
{
    std::lock_guard lock(this->mutex_);
    return this->shutdown_;
}
}  // namespace chatterino
