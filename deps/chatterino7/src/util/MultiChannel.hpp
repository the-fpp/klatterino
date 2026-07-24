#pragma once

#include "common/Channel.hpp"
#include "messages/MessageDraft.hpp"
#include "util/MultiChannelIndicatorMode.hpp"
#include "util/MultiChannelRouting.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QSet>

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace chatterino {

struct ChildChannelDescriptor;
enum class MessagePlatform : uint8_t;

class MultiChannel : public Channel
{
public:
    enum class Platform : uint8_t {
        Twitch = 0,
        Kick = 1,
        Rumble = 2,
    };

    struct Spec {
        Platform platform = Platform::Twitch;
        QString name;
        std::optional<ChannelLayoutIdentity> layoutIdentity;

        ChildChannelDescriptor descriptor() const;
        static std::optional<Spec> fromDescriptor(
            const ChildChannelDescriptor &descriptor);
        friend QDataStream &operator<<(QDataStream &stream, const Spec &spec);
        friend QDataStream &operator>>(QDataStream &stream, Spec &spec);
    };

    /// Construction-time resolver used by deterministic integration tests and
    /// alternate in-memory channel registries. The resolver is consumed while
    /// constructing the immutable child snapshot and is never retained.
    using ChannelResolver = std::function<ChannelPtr(const Spec &)>;

    MultiChannel(std::span<const Spec> channels,
                 MultiChannelIndicatorMode indicatorMode);
    MultiChannel(std::span<const Spec> channels,
                 MultiChannelIndicatorMode indicatorMode,
                 ChannelResolver channelResolver);

    /// Per-child bounded replay filter for provider message IDs. The 50,000-ID
    /// default matches the repository's other long-lived FIFO deduplication
    /// windows while preventing a busy split from retaining IDs forever.
    /// Duplicate observations do not refresh FIFO order.
    class LiveMessageIdDeduplicator
    {
    public:
        static constexpr size_t DEFAULT_CAPACITY = 50'000;

        explicit LiveMessageIdDeduplicator(
            size_t capacity = DEFAULT_CAPACITY) noexcept;
        LiveMessageIdDeduplicator(const LiveMessageIdDeduplicator &) = delete;
        LiveMessageIdDeduplicator &operator=(
            const LiveMessageIdDeduplicator &) = delete;
        LiveMessageIdDeduplicator(
            LiveMessageIdDeduplicator &&) noexcept = default;
        LiveMessageIdDeduplicator &operator=(
            LiveMessageIdDeduplicator &&) noexcept = default;

        /// Returns true only when id was not in the retained FIFO window.
        bool remember(QString id);
        bool contains(const QString &id) const;
        size_t size() const noexcept;

    private:
        size_t capacity_;
        QSet<QString> ids_;
        std::deque<QString> fifo_;
    };

    struct ChildChannel {
        Platform platform;
        ChannelPtr channel;
        Spec originalSpec;
        bool primaryRuntimeChannel = true;
        bool rumbleRoutingSessionAvailable = false;
        std::vector<pajlada::Signals::ScopedConnection> connections;
        uint64_t activitySequence = 0;
        LiveMessageIdDeduplicator observedLiveMessageIds;

        Spec spec() const;
        ChildChannelDescriptor descriptor() const;
    };

    struct DraftDispatchResult {
        std::optional<size_t> destinationIndex;
        ChannelPtr destination;
        QString destinationPlatform;
        QString destinationName;
        bool usedFallback = false;
        std::vector<MessageDraftEvaluation> evaluations;

        bool sent() const noexcept
        {
            return this->destinationIndex.has_value();
        }
    };

    std::span<const ChildChannel> channels() const;
    /// Whether an explicit routing override currently has a usable
    /// destination. Twitch and Kick preserve their existing presence-based
    /// behavior. Rumble is eligible only after entering a resolved live-chat
    /// connection lifecycle, including its recoverable reconnect/backoff
    /// states.
    bool isRoutingPlatformAvailable(Platform platform) const;

    const QString &getDisplayName() const override;
    const QString &getLocalizedName() const override;

    pajlada::Signals::NoArgSignal activeChannelChanged;
    /// Forwarded child state changes used by aggregate views and tab chrome.
    /// Child connections are owned by this immutable aggregate; Split and
    /// ChannelView detach their aggregate-facing holders on replacement.
    pajlada::Signals::NoArgSignal childStateChanged;
    pajlada::Signals::Signal<const QString &> messageHistoryLoadFailed;
    const ChildChannel *activeChannel() const;
    size_t activeChannelIndex() const;
    void setActiveChannelIndex(size_t index);
    bool initialHistorySettled() const
    {
        return !this->historyTransactionPending_ &&
               !this->historyTransactionAborted_;
    }

    bool isEmpty() const override;

    bool canSendMessage() const override;
    bool isWritable() const override;
    void sendMessage(const QString &message) override;
    DraftDispatchResult sendMessageDraft(
        const MessageDraft &draft, size_t primaryIndex,
        MultiChannelRoutePolicy policy =
            MultiChannelRoutePolicy::CompatibleFallback);
    using DraftSendCallback =
        std::function<void(DraftDispatchResult, Channel::SendResult)>;
    DraftDispatchResult sendMessageDraftAsync(
        const MessageDraft &draft, const QString &sendText,
        size_t primaryIndex, MultiChannelRoutePolicy policy,
        DraftSendCallback callback);
    DraftDispatchResult sendMessageDraft(
        const MessageDraft &draft, const QString &sendText,
        size_t primaryIndex,
        MultiChannelRoutePolicy policy =
            MultiChannelRoutePolicy::CompatibleFallback);
    /// Resolves a draft against one immutable child-state snapshot without
    /// sending it. SplitInput uses this for the pending-destination indicator,
    /// so preview and submission share the exact same routing implementation.
    DraftDispatchResult previewMessageDraftDestination(
        const MessageDraft &draft, const QString &sendText, size_t primaryIndex,
        MultiChannelRoutePolicy policy =
            MultiChannelRoutePolicy::CompatibleFallback) const;
    bool isMod() const override;
    bool isBroadcaster() const override;
    bool hasModRights() const override;
    bool hasHighRateLimit() const override;
    bool isLive() const override;
    bool isRerun() const override;
    bool shouldIgnoreHighlights() const override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;

    MultiChannelIndicatorMode indicatorMode() const;

private:
    DraftDispatchResult selectMessageDraftDestination(
        const MessageDraft &draft, const QString &sendText, size_t primaryIndex,
        MultiChannelRoutePolicy policy) const;
    void refreshDisplayName();
    void setComputedName(const QString &name);
    void forwardOrBufferHistory(size_t childIndex,
                                const std::vector<MessagePtr> &messages);
    void evaluateHistoryTransaction();
    void commitHistoryTransaction();
    void abortHistoryTransaction(size_t failedChild, const QString &error);
    void showHistoryFailureIfActive(const QString &message) const;
    void observeLiveMessage(
        size_t childIndex, const MessagePtr &message,
        const std::optional<MessageFlags> &overridingFlags,
        bool idAlreadyRemembered = false);
    bool rememberMessageId(size_t childIndex, const MessagePtr &message);

    QString uniqueName;
    QString computedName;
    std::vector<ChildChannel> channels_;
    size_t activeChannel_ = 0;
    uint64_t nextActivitySequence_ = 0;

    bool historyTransactionPending_ = false;
    bool historyTransactionAborted_ = false;
    std::vector<std::vector<MessagePtr>> bufferedHistory_;

    MultiChannelIndicatorMode indicatorMode_ =
        MultiChannelIndicatorMode::PlatformBadgeIfUnselected;
};

bool platformMatches(MessagePlatform lhs, MultiChannel::Platform rhs) noexcept;
QString multiChannelChildDisplayName(const MultiChannel::ChildChannel &child);
bool multiChannelChildMatches(const MultiChannel::ChildChannel &child,
                              MessagePlatform platform,
                              QStringView sourceName) noexcept;

}  // namespace chatterino

Q_DECLARE_METATYPE(chatterino::MultiChannel::Platform)
Q_DECLARE_METATYPE(chatterino::MultiChannel::Spec)
