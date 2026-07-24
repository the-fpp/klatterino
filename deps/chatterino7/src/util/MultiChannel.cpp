#include "util/MultiChannel.hpp"

#include "Application.hpp"
#include "common/WindowDescriptors.hpp"
#include "messages/Message.hpp"
#include "providers/history/MessageHistoryLoadRegistry.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/rumble/RumbleApplicationController.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleLayoutLocator.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/QCompareTransparent.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/splits/Split.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QUuid>
#include <QVarLengthArray>

#include <algorithm>
#include <set>

namespace {

using namespace chatterino;
using namespace Qt::Literals;

constexpr int MAX_HISTORY_LOAD_ATTEMPTS = 3;

QString makeChannelName(const std::set<QString, QCompareCaseInsensitive> &known,
                        bool space)
{
    if (known.empty())
    {
        return u"Multichannel[empty]"_s;
    }

    bool first = true;
    QString combined;
    for (const auto &name : known)
    {
        if (first)
        {
            first = false;
        }
        else
        {
            combined += u',';
            if (space)
            {
                combined += u' ';
            }
        }
        combined += name;
    }
    return combined;
}

QString makeChannelName(std::span<const MultiChannel::Spec> specs, bool space)
{
    std::set<QString, QCompareCaseInsensitive> known;
    for (const auto &spec : specs)
    {
        known.emplace(spec.name);
    }
    return makeChannelName(known, space);
}

QString stableChildName(const MultiChannel::ChildChannel &child)
{
    if (child.platform == MultiChannel::Platform::Rumble)
    {
        return child.channel->getDisplayName();
    }
    return child.channel->getName();
}

QString makeChannelName(std::span<const MultiChannel::ChildChannel> specs,
                        bool space)
{
    std::set<QString, QCompareCaseInsensitive> known;
    for (const auto &spec : specs)
    {
        known.emplace(stableChildName(spec));
    }
    return makeChannelName(known, space);
}

std::optional<RumbleLayoutLocator> explicitPublicRumbleUrl(const QString &input)
{
    auto locator = RumbleLayoutLocator::fromPersisted(input);
    if (!locator)
    {
        return std::nullopt;
    }
    return *std::move(locator);
}

std::optional<RumbleLayoutLocator> persistedRumbleLocator(
    const std::optional<ChannelLayoutIdentity> &identity,
    const QString &legacyName)
{
    if (identity && identity->platform == QStringLiteral("rumble"))
    {
        auto locator = RumbleLayoutLocator::fromPersisted(identity->locator);
        if (locator)
        {
            return *std::move(locator);
        }
    }
    return explicitPublicRumbleUrl(legacyName);
}

MultiChannel::Spec normalizedSpec(const MultiChannel::Spec &spec)
{
    if (spec.platform != MultiChannel::Platform::Rumble)
    {
        return spec;
    }

    auto normalized = spec;
    const auto locator = persistedRumbleLocator(spec.layoutIdentity, spec.name);
    normalized.name = locator ? locator->canonicalUrl() : QString{};
    normalized.layoutIdentity = ChannelLayoutIdentity{
        .platform = QStringLiteral("rumble"),
        .locator = normalized.name,
    };
    return normalized;
}

ChannelPtr resolveChannel(const MultiChannel::Spec &spec)
{
    switch (spec.platform)
    {
        case MultiChannel::Platform::Twitch:
            return getApp()->getTwitch()->getOrAddChannel(spec.name);
        case MultiChannel::Platform::Kick:
            return getApp()->getKickChatServer()->getOrCreate(spec.name);
        case MultiChannel::Platform::Rumble:
            if (auto *controller = getApp()->getRumble())
            {
                const auto locator =
                    spec.layoutIdentity && spec.layoutIdentity->platform ==
                                               QStringLiteral("rumble")
                        ? spec.layoutIdentity->locator
                        : spec.name;
                return controller->restore(locator).get();
            }
            auto normalized = RumbleLayoutLocator::fromPersisted(
                spec.layoutIdentity ? spec.layoutIdentity->locator : spec.name);
            return makeRumbleLayoutPlaceholder(
                       normalized ? normalized->canonicalUrl() : QString{})
                .get();
    }
    return Channel::getEmpty();
}

bool isHistoryMessage(const MessagePtr &message)
{
    return message && message->flags.has(MessageFlag::RecentMessage);
}

bool isProviderIngressLiveChat(
    const MessagePtr &message,
    const std::optional<MessageFlags> &overridingFlags)
{
    if (!message || message->id.isEmpty() || message->loginName.isEmpty())
    {
        return false;
    }

    // Overrides describe the flags used by the forwarded/reposted view. Keep
    // original exclusions too: an override must never turn a system, replay,
    // debug, or whisper message into provider live-chat activity.
    auto effectiveFlags = message->flags;
    if (overridingFlags)
    {
        effectiveFlags.set(*overridingFlags);
    }

    return !effectiveFlags.hasAny(
        {MessageFlag::System, MessageFlag::RecentMessage,
         MessageFlag::DoNotLog, MessageFlag::Timeout, MessageFlag::Untimeout,
         MessageFlag::PubSub, MessageFlag::Subscription, MessageFlag::AutoMod,
         MessageFlag::ClearChat, MessageFlag::EventSub, MessageFlag::Debug,
         MessageFlag::ModerationAction, MessageFlag::Whisper});
}

}  // namespace

namespace chatterino {

MultiChannel::LiveMessageIdDeduplicator::LiveMessageIdDeduplicator(
    size_t capacity) noexcept
    : capacity_(capacity)
{
}

bool MultiChannel::LiveMessageIdDeduplicator::remember(QString id)
{
    if (id.isEmpty() || this->ids_.contains(id))
    {
        return false;
    }

    if (this->capacity_ == 0)
    {
        return true;
    }

    this->ids_.insert(id);
    this->fifo_.push_back(std::move(id));
    if (this->fifo_.size() > this->capacity_)
    {
        this->ids_.remove(this->fifo_.front());
        this->fifo_.pop_front();
    }
    return true;
}

bool MultiChannel::LiveMessageIdDeduplicator::contains(
    const QString &id) const
{
    return this->ids_.contains(id);
}

size_t MultiChannel::LiveMessageIdDeduplicator::size() const noexcept
{
    return static_cast<size_t>(this->ids_.size());
}

MultiChannel::Spec MultiChannel::ChildChannel::spec() const
{
    if (this->platform == Platform::Rumble)
    {
        return this->originalSpec;
    }
    return {
        .platform = this->platform,
        .name = this->channel->getName(),
    };
}

ChildChannelDescriptor MultiChannel::ChildChannel::descriptor() const
{
    return this->spec().descriptor();
}

ChildChannelDescriptor MultiChannel::Spec::descriptor() const
{
    const auto normalized = normalizedSpec(*this);
    return ChildChannelDescriptor{
        .platform = qmagicenum::enumNameString(this->platform),
        .channelName = normalized.name,
        .layoutIdentity = normalized.layoutIdentity,
    };
}

std::optional<MultiChannel::Spec> MultiChannel::Spec::fromDescriptor(
    const ChildChannelDescriptor &descriptor)
{
    auto platform = qmagicenum::enumCast<Platform>(descriptor.platform);
    if (!platform && descriptor.platform.compare(QStringLiteral("rumble"),
                                                 Qt::CaseInsensitive) == 0)
    {
        platform = Platform::Rumble;
    }
    if (!platform)
    {
        return std::nullopt;
    }
    return normalizedSpec(MultiChannel::Spec{
        .platform = *platform,
        .name = descriptor.channelName,
        .layoutIdentity = descriptor.layoutIdentity,
    });
}

QDataStream &operator<<(QDataStream &stream, const MultiChannel::Spec &spec)
{
    stream << spec.platform;
    if (spec.platform == MultiChannel::Platform::Rumble)
    {
        const auto locator =
            persistedRumbleLocator(spec.layoutIdentity, spec.name);
        stream << (locator ? locator->canonicalUrl() : QString{});
    }
    else
    {
        stream << spec.name;
    }
    return stream;
}

QDataStream &operator>>(QDataStream &stream, MultiChannel::Spec &spec)
{
    stream >> spec.platform;
    stream >> spec.name;
    spec.layoutIdentity.reset();
    if (spec.platform == MultiChannel::Platform::Rumble)
    {
        auto locator = explicitPublicRumbleUrl(spec.name);
        spec.name = locator ? locator->canonicalUrl() : QString{};
        spec.layoutIdentity = ChannelLayoutIdentity{
            .platform = QStringLiteral("rumble"),
            .locator = spec.name,
        };
    }
    return stream;
}

MultiChannel::MultiChannel(std::span<const Spec> channels,
                           MultiChannelIndicatorMode indicatorMode)
    : MultiChannel(channels, indicatorMode, resolveChannel)
{
}

MultiChannel::MultiChannel(std::span<const Spec> channels,
                           MultiChannelIndicatorMode indicatorMode,
                           ChannelResolver channelResolver)
    : Channel(makeChannelName(channels, false), Type::Multi)
    , historyTransactionPending_(
          getSettings()->loadTwitchMessageHistoryOnConnect)
    , bufferedHistory_(channels.size())
    , indicatorMode_(indicatorMode)
{
    for (const auto &spec : channels)
    {
        auto storedSpec = normalizedSpec(spec);
        const size_t childIndex = this->channels_.size();
        ChannelPtr channel;
        if (channelResolver)
        {
            channel = channelResolver(storedSpec);
        }
        if (!channel)
        {
            channel = Channel::getEmpty();
        }
        const bool primaryRuntimeChannel =
            storedSpec.platform != Platform::Rumble ||
            std::ranges::none_of(this->channels_, [&](const auto &existing) {
                return existing.platform == Platform::Rumble &&
                       existing.channel.get() == channel.get();
            });
        std::vector<pajlada::Signals::ScopedConnection> connections;
        if (primaryRuntimeChannel)
        {
            connections.emplace_back(channel->messageAppended.connect(
                [this, childIndex](const auto &ptr, const auto &flags) {
                    const bool rumble =
                        this->channels_[childIndex].platform == Platform::Rumble;
                    const bool hasStableId = ptr && !ptr->id.isEmpty();
                    if (rumble && hasStableId &&
                        !this->rememberMessageId(childIndex, ptr))
                    {
                        return;
                    }
                    this->observeLiveMessage(childIndex, ptr, flags,
                                             rumble && hasStableId);
                    if (rumble)
                    {
                        // Rumble bootstrap batches can race realtime appends and
                        // other providers. Use the chronological insertion path
                        // so the aggregate stays ordered and ID-deduplicated.
                        this->fillInMissingMessages({ptr});
                    }
                    else
                    {
                        this->addMessage(ptr, MessageContext::Repost, flags);
                    }
                }));
            connections.emplace_back(channel->messagesAddedAtStart.connect(
                [this, childIndex](const auto &msgs) {
                    this->forwardOrBufferHistory(childIndex, msgs);
                }));
            connections.emplace_back(channel->messageReplaced.connect(
                [this](size_t idx, const MessagePtr &prev,
                       const MessagePtr &replaced) {
                    this->replaceMessage(idx, prev, replaced);
                }));
            connections.emplace_back(channel->filledInMessages.connect(
                [this, childIndex](const auto &msgs) {
                    this->forwardOrBufferHistory(childIndex, msgs);
                }));
            connections.emplace_back(
                channel->displayNameChanged.connect([this] {
                    this->refreshDisplayName();
                }));
            if (storedSpec.platform == Platform::Rumble)
            {
                if (auto rumble =
                        std::dynamic_pointer_cast<RumbleChannel>(channel))
                {
                    connections.emplace_back(rumble->stateChanged.connect(
                        [this, runtimeChannel = channel.get()](
                            RumbleChannelState, RumbleChannelState current) {
                            for (auto &child : this->channels_)
                            {
                                if (child.platform != Platform::Rumble ||
                                    child.channel.get() != runtimeChannel)
                                {
                                    continue;
                                }
                                child.rumbleRoutingSessionAvailable =
                                    current != RumbleChannelState::Failed &&
                                    current != RumbleChannelState::Closed &&
                                    child.channel && child.channel->isLive();
                            }
                            this->childStateChanged.invoke();
                        }));
                    connections.emplace_back(
                        rumble->lifecycleMetadataChanged.connect([this] {
                            this->childStateChanged.invoke();
                        }));
                    connections.emplace_back(
                        rumble->reconnectAvailabilityChanged.connect([this] {
                            this->childStateChanged.invoke();
                        }));
                }
            }
            if (storedSpec.platform != Platform::Rumble)
            {
                connections.emplace_back(
                    messagehistory::Registry::instance().connect(
                        channel,
                        [this](messagehistory::State, const QString &) {
                            this->evaluateHistoryTransaction();
                        }));
            }
        }
        // Ignore messagesCleared - we'd need to figure out which messages to clear.

        bool rumbleRoutingSessionAvailable = false;
        if (const auto rumble =
                std::dynamic_pointer_cast<RumbleChannel>(channel))
        {
            rumbleRoutingSessionAvailable =
                rumble->state() != RumbleChannelState::Failed &&
                rumble->state() != RumbleChannelState::Closed &&
                rumble->isLive();
        }
        this->channels_.emplace_back(ChildChannel{
            .platform = storedSpec.platform,
            .channel = std::move(channel),
            .originalSpec = std::move(storedSpec),
            .primaryRuntimeChannel = primaryRuntimeChannel,
            .rumbleRoutingSessionAvailable = rumbleRoutingSessionAvailable,
            .connections = std::move(connections),
        });
    }
    this->refreshDisplayName();

    QVarLengthArray<std::vector<MessagePtr>, 4> snapshots;
    for (size_t i = 0; i < this->channels_.size(); ++i)
    {
        if (!this->channels_[i].primaryRuntimeChannel)
        {
            snapshots.emplace_back();
            continue;
        }
        auto snapshot = this->channels_[i].channel->getMessageSnapshot();
        snapshots.emplace_back();
        snapshots.back().reserve(snapshot.size());
        for (const auto &message : snapshot)
        {
            const bool firstObservation = this->rememberMessageId(i, message);
            if (this->channels_[i].platform == Platform::Rumble && message &&
                !message->id.isEmpty() && !firstObservation)
            {
                continue;
            }
            if (!this->historyTransactionPending_)
            {
                snapshots.back().push_back(message);
                continue;
            }
            if (isHistoryMessage(message))
            {
                this->bufferedHistory_[i].push_back(message);
            }
            else
            {
                snapshots.back().push_back(message);
            }
        }
    }

    QVarLengthArray<std::span<const MessagePtr>, 4> snapshotViews;
    for (const auto &snapshot : snapshots)
    {
        snapshotViews.emplace_back(snapshot);
    }
    this->mergeFrom(snapshotViews);
    this->evaluateHistoryTransaction();
}

const QString &MultiChannel::getDisplayName() const
{
    return this->computedName;
}

const QString &MultiChannel::getLocalizedName() const
{
    return this->computedName;
}

std::span<const MultiChannel::ChildChannel> MultiChannel::channels() const
{
    return this->channels_;
}

bool MultiChannel::isRoutingPlatformAvailable(Platform platform) const
{
    return std::ranges::any_of(this->channels_, [platform](const auto &child) {
        if (child.platform != platform)
        {
            return false;
        }
        if (platform != Platform::Rumble)
        {
            return true;
        }

        if (!std::dynamic_pointer_cast<RumbleChannel>(child.channel))
        {
            // Deterministic alternate resolvers use provider-shaped test
            // channels. Their live contract is the closest equivalent.
            return child.channel && child.channel->isLive();
        }
        return child.rumbleRoutingSessionAvailable;
    });
}

const MultiChannel::ChildChannel *MultiChannel::activeChannel() const
{
    if (this->activeChannel_ >= this->channels_.size())
    {
        return nullptr;
    }
    return &this->channels_[this->activeChannel_];
}

size_t MultiChannel::activeChannelIndex() const
{
    return this->activeChannel_;
}

void MultiChannel::setActiveChannelIndex(size_t index)
{
    const auto effective = this->channels_.empty()
                               ? size_t{0}
                               : std::min(index, this->channels_.size() - 1);
    if (this->activeChannel_ == effective)
    {
        return;
    }
    this->activeChannel_ = effective;
    this->activeChannelChanged.invoke();
}

bool MultiChannel::isEmpty() const
{
    return this->channels_.empty();
}

bool MultiChannel::canSendMessage() const
{
    return std::ranges::any_of(this->channels_, [](const auto &child) {
        return child.channel->canSendMessage();
    });
}

bool MultiChannel::isWritable() const
{
    return std::ranges::any_of(this->channels_, [](const auto &child) {
        return child.channel->isWritable();
    });
}

void MultiChannel::sendMessage(const QString &message)
{
    const auto *active = this->activeChannel();
    if (active)
    {
        active->channel->sendMessage(message);
    }
}

MultiChannel::DraftDispatchResult MultiChannel::sendMessageDraft(
    const MessageDraft &draft, size_t primaryIndex,
    MultiChannelRoutePolicy policy)
{
    return this->sendMessageDraft(draft, draft.text, primaryIndex, policy);
}

MultiChannel::DraftDispatchResult MultiChannel::sendMessageDraft(
    const MessageDraft &draft, const QString &sendText, size_t primaryIndex,
    MultiChannelRoutePolicy policy)
{
    auto result = this->selectMessageDraftDestination(
        draft, sendText, primaryIndex, policy);
    if (result.destination)
        result.destination->sendMessage(sendText);
    return result;
}

MultiChannel::DraftDispatchResult MultiChannel::selectMessageDraftDestination(
    const MessageDraft &draft, const QString &sendText, size_t primaryIndex,
    MultiChannelRoutePolicy policy) const
{
    DraftDispatchResult result;
    result.evaluations.reserve(this->channels_.size());

    std::vector<ChannelPtr> destinations;
    std::vector<QString> destinationPlatforms;
    std::vector<QString> destinationNames;
    std::vector<MessageSendContext> contexts;
    std::vector<MultiChannelRouteCandidate> candidates;
    destinations.reserve(this->channels_.size());
    destinationPlatforms.reserve(this->channels_.size());
    destinationNames.reserve(this->channels_.size());
    contexts.reserve(this->channels_.size());
    candidates.reserve(this->channels_.size());

    // Capture one immutable routing snapshot before selecting or dispatching.
    // The strong references also keep the chosen child alive for the one send.
    for (const auto &child : this->channels_)
    {
        destinations.push_back(child.channel);
        auto context = child.channel->messageSendContext();
        destinationPlatforms.push_back(context.platform);
        destinationNames.push_back(child.channel->getDisplayName());
        contexts.push_back(std::move(context));
    }

    // Ordinary multi-channel text is resolved against the union of every
    // child's current emote capabilities. Explicit completion/picker ranges
    // remain exact, while untracked tokens can select the destination that can
    // actually render them. Provider commands and replies stay primary-bound.
    const auto dictionary = messageDraftCandidates(contexts);
    auto routingDraft = draft;
    if (policy == MultiChannelRoutePolicy::CompatibleFallback)
    {
        if (draft.destinationPlatformOverride)
        {
            // A platform override is intentionally stronger than existing
            // emote provenance. Preserve malformed/unresolved provenance as a
            // hard failure, but otherwise reinterpret rendered tokens against
            // the selected platform instead of letting an earlier completion
            // escape or veto the explicit destination.
            routingDraft.emotes.clear();
        }
        routingDraft = MessageDraft::reconstructUntracked(
            std::move(routingDraft), dictionary);
    }

    for (size_t index = 0; index < this->channels_.size(); ++index)
    {
        const auto &child = this->channels_[index];
        auto evaluation = chatterino::evaluateMessageDraft(
            routingDraft, contexts[index], sendText);
        const bool platformAllowed =
            !draft.destinationPlatformOverride ||
            contexts[index].platform.compare(*draft.destinationPlatformOverride,
                                             Qt::CaseInsensitive) == 0;
        candidates.push_back({
            .sendable = platformAllowed &&
                        (policy == MultiChannelRoutePolicy::CompatibleFallback
                             ? evaluation.sendableWithInferredEmoteFallback
                             : evaluation.sendable),
            .supportedEmoteOccurrences = evaluation.supportedEmoteOccurrences,
            .activitySequence = child.activitySequence,
        });
        result.evaluations.push_back(std::move(evaluation));
    }

    const auto selected =
        selectMultiChannelDestination(candidates, primaryIndex, policy);
    if (!selected)
    {
        return result;
    }

    const auto index = *selected;
    auto destination = destinations[index];

    result.destinationIndex = index;
    result.destination = destination;
    result.destinationPlatform = std::move(destinationPlatforms[index]);
    result.destinationName = std::move(destinationNames[index]);
    result.usedFallback = index != primaryIndex;

    return result;
}

MultiChannel::DraftDispatchResult MultiChannel::previewMessageDraftDestination(
    const MessageDraft &draft, const QString &sendText, size_t primaryIndex,
    MultiChannelRoutePolicy policy) const
{
    return this->selectMessageDraftDestination(draft, sendText, primaryIndex,
                                               policy);
}

MultiChannel::DraftDispatchResult MultiChannel::sendMessageDraftAsync(
    const MessageDraft &draft, const QString &sendText, size_t primaryIndex,
    MultiChannelRoutePolicy policy, DraftSendCallback callback)
{
    auto result = this->selectMessageDraftDestination(
        draft, sendText, primaryIndex, policy);
    if (!result.destination)
    {
        return result;
    }
    auto immediate = result;
    auto destination = result.destination;
    destination->sendMessageAsync(
        sendText,
        [result = std::move(result), callback = std::move(callback)](
            Channel::SendResult sendResult) mutable {
            if (callback)
                callback(std::move(result), std::move(sendResult));
        });
    return immediate;
}

bool MultiChannel::isMod() const
{
    const auto *active = this->activeChannel();
    if (active)
    {
        return active->channel->isMod();
    }
    return false;
}

bool MultiChannel::isBroadcaster() const
{
    const auto *active = this->activeChannel();
    if (active)
    {
        return active->channel->isBroadcaster();
    }
    return false;
}

bool MultiChannel::hasModRights() const
{
    const auto *active = this->activeChannel();
    if (active)
    {
        return active->channel->hasModRights();
    }
    return false;
}

bool MultiChannel::hasHighRateLimit() const
{
    const auto *active = this->activeChannel();
    if (active)
    {
        return active->channel->hasHighRateLimit();
    }
    return false;
}

bool MultiChannel::isLive() const
{
    return std::ranges::any_of(this->channels_, [](const auto &c) {
        return c.channel->isLive();
    });
}

bool MultiChannel::isRerun() const
{
    return std::ranges::any_of(this->channels_, [](const auto &c) {
        return c.channel->isRerun();
    });
}

bool MultiChannel::shouldIgnoreHighlights() const
{
    const auto *active = this->activeChannel();
    if (active)
    {
        return active->channel->shouldIgnoreHighlights();
    }
    return false;
}

bool MultiChannel::canReconnect() const
{
    return true;
}

void MultiChannel::reconnect()
{
    for (const auto &chan : this->channels_)
    {
        if (chan.platform == Platform::Rumble)
        {
            if (!chan.primaryRuntimeChannel)
            {
                continue;
            }
            const auto runtime =
                std::dynamic_pointer_cast<RumbleChannel>(chan.channel);
            if (runtime && runtime->canReconnect())
            {
                // A resolved shared runtime owns one reconnect delegate. Use
                // it directly so locator aliases cannot enqueue duplicate
                // generations; placeholders fall through to locator repair.
                chan.channel->reconnect();
                continue;
            }
            const auto locator = persistedRumbleLocator(
                chan.originalSpec.layoutIdentity, chan.originalSpec.name);
            if (auto *controller = getApp()->getRumble(); controller && locator)
            {
                controller->retry(locator->canonicalUrl());
            }
            continue;
        }
        chan.channel->reconnect();
    }
}

QString MultiChannel::getCurrentStreamID() const
{
    const auto *active = this->activeChannel();
    if (active)
    {
        return active->channel->getCurrentStreamID();
    }
    return {};
}

MultiChannelIndicatorMode MultiChannel::indicatorMode() const
{
    return this->indicatorMode_;
}

void MultiChannel::refreshDisplayName()
{
    if (this->channels_.empty())
    {
        this->setComputedName(u"empty"_s);
        return;
    }
    this->setComputedName(makeChannelName(this->channels_, true));
}

void MultiChannel::setComputedName(const QString &name)
{
    if (this->computedName == name)
    {
        return;
    }
    this->computedName = name;
    this->displayNameChanged.invoke();
}

void MultiChannel::forwardOrBufferHistory(
    size_t childIndex, const std::vector<MessagePtr> &messages)
{
    if (messages.empty())
    {
        return;
    }

    std::vector<MessagePtr> visible;
    visible.reserve(messages.size());
    for (const auto &message : messages)
    {
        if (childIndex < this->channels_.size() &&
            this->channels_[childIndex].platform == Platform::Rumble &&
            message && !message->id.isEmpty() &&
            !this->rememberMessageId(childIndex, message))
        {
            // Bootstrap/reconnect history participates in the same bounded
            // per-runtime replay window as realtime appends. Realtime-first
            // overlap is discarded before reaching the aggregate insertion.
            continue;
        }
        if (!isHistoryMessage(message))
        {
            visible.push_back(message);
            continue;
        }

        if (this->historyTransactionPending_)
        {
            if (childIndex < this->bufferedHistory_.size())
            {
                this->bufferedHistory_[childIndex].push_back(message);
            }
        }
        else if (!this->historyTransactionAborted_)
        {
            visible.push_back(message);
        }
    }

    if (!visible.empty())
    {
        this->fillInMissingMessages(visible);
    }
}

void MultiChannel::evaluateHistoryTransaction()
{
    if (this->historyTransactionAborted_)
    {
        const bool allInitialLoadsFinished =
            std::ranges::all_of(this->channels_, [](const auto &child) {
                if (child.platform == Platform::Rumble)
                {
                    return true;
                }
                const auto state =
                    messagehistory::Registry::instance().state(child.channel);
                return state == messagehistory::State::Loaded ||
                       state == messagehistory::State::Failed;
            });
        if (allInitialLoadsFinished)
        {
            this->historyTransactionAborted_ = false;
        }
        return;
    }

    if (!this->historyTransactionPending_)
    {
        return;
    }

    bool allLoaded = true;
    for (size_t i = 0; i < this->channels_.size(); ++i)
    {
        if (this->channels_[i].platform == Platform::Rumble)
        {
            continue;
        }
        const auto state = messagehistory::Registry::instance().state(
            this->channels_[i].channel);
        if (state == messagehistory::State::Failed)
        {
            this->abortHistoryTransaction(
                i, messagehistory::Registry::instance().error(
                       this->channels_[i].channel));
            this->evaluateHistoryTransaction();
            return;
        }
        if (state != messagehistory::State::Loaded)
        {
            allLoaded = false;
        }
    }

    if (allLoaded)
    {
        this->commitHistoryTransaction();
    }
}

void MultiChannel::commitHistoryTransaction()
{
    if (!this->historyTransactionPending_)
    {
        return;
    }

    std::vector<MessagePtr> history;
    for (auto &buffer : this->bufferedHistory_)
    {
        history.insert(history.end(), buffer.begin(), buffer.end());
        buffer.clear();
    }

    // Channel::fillInMissingMessages requires ascending input. Keep the
    // concatenation order as the deterministic tie-breaker: child order first,
    // then each provider's source order.
    std::stable_sort(history.begin(), history.end(),
                     [](const auto &left, const auto &right) {
                         return left->serverReceivedTime <
                                right->serverReceivedTime;
                     });

    this->historyTransactionPending_ = false;
    this->historyTransactionAborted_ = false;
    if (!history.empty())
    {
        this->fillInMissingMessages(history);
    }
}

void MultiChannel::abortHistoryTransaction(size_t failedChild,
                                            const QString &error)
{
    if (!this->historyTransactionPending_ ||
        failedChild >= this->channels_.size())
    {
        return;
    }

    for (auto &buffer : this->bufferedHistory_)
    {
        buffer.clear();
    }
    this->historyTransactionPending_ = false;
    this->historyTransactionAborted_ = true;

    const auto &child = this->channels_[failedChild];
    const QString platform = qmagicenum::enumNameString(child.platform);
    const QString message =
        QStringLiteral(
            "No message history was added to this combined tab because %1 "
            "channel %2 failed after %3 attempts.\n\n%4")
            .arg(platform, stableChildName(child))
            .arg(MAX_HISTORY_LOAD_ATTEMPTS)
            .arg(error);

    this->messageHistoryLoadFailed.invoke(message);
    this->showHistoryFailureIfActive(message);
}

void MultiChannel::showHistoryFailureIfActive(const QString &message) const
{
    for (auto *widget : QApplication::allWidgets())
    {
        auto *split = qobject_cast<Split *>(widget);
        if (split == nullptr || !split->isVisible())
        {
            continue;
        }
        if (split->getChannel().get() != this)
        {
            continue;
        }

        QMessageBox::warning(split, QStringLiteral("Message history unavailable"),
                             message);
        return;
    }
}

bool MultiChannel::rememberMessageId(size_t childIndex,
                                     const MessagePtr &message)
{
    if (childIndex >= this->channels_.size() || !message ||
        message->id.isEmpty())
    {
        return false;
    }

    return this->channels_[childIndex].observedLiveMessageIds.remember(
        message->id);
}

void MultiChannel::observeLiveMessage(
    size_t childIndex, const MessagePtr &message,
    const std::optional<MessageFlags> &overridingFlags,
    bool idAlreadyRemembered)
{
    if (childIndex >= this->channels_.size() ||
        !isProviderIngressLiveChat(message, overridingFlags))
    {
        return;
    }

    auto &child = this->channels_[childIndex];
    if (!idAlreadyRemembered && !this->rememberMessageId(childIndex, message))
    {
        return;
    }
    child.activitySequence = ++this->nextActivitySequence_;
}

bool platformMatches(MessagePlatform lhs, MultiChannel::Platform rhs) noexcept
{
    switch (lhs)
    {
        case MessagePlatform::AnyOrTwitch:
            return rhs == MultiChannel::Platform::Twitch;
        case MessagePlatform::Kick:
            return rhs == MultiChannel::Platform::Kick;
        case MessagePlatform::Rumble:
            return rhs == MultiChannel::Platform::Rumble;
    }
    return false;
}

QString multiChannelChildDisplayName(const MultiChannel::ChildChannel &child)
{
    return stableChildName(child);
}

bool multiChannelChildMatches(const MultiChannel::ChildChannel &child,
                              MessagePlatform platform,
                              QStringView sourceName) noexcept
{
    if (!child.channel || !platformMatches(platform, child.platform))
    {
        return false;
    }
    if (child.platform != MultiChannel::Platform::Rumble)
    {
        return sourceName.compare(child.channel->getName(),
                                  Qt::CaseInsensitive) == 0;
    }
    if (!child.primaryRuntimeChannel)
    {
        return false;
    }

    const auto rumble =
        std::dynamic_pointer_cast<const RumbleChannel>(child.channel);
    if (!rumble || !rumble->metadata())
    {
        return false;
    }
    const auto &metadata = *rumble->metadata();
    const auto matches = [&](const std::optional<RumbleChannelKey> &key) {
        return key &&
               sourceName.compare(key->value(), Qt::CaseInsensitive) == 0;
    };
    return matches(metadata.channelSlug()) || matches(metadata.embedId()) ||
           matches(metadata.streamId()) ||
           sourceName.compare(metadata.displayName(), Qt::CaseInsensitive) == 0;
}

}  // namespace chatterino
