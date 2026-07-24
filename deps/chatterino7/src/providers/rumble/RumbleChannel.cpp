// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT
#include "providers/rumble/RumbleChannel.hpp"

#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/rumble/RumbleDiagnostics.hpp"
#include "providers/rumble/RumbleEvent.hpp"
#include "providers/rumble/RumbleMessageBuilder.hpp"
#include "providers/rumble/RumbleSession.hpp"

#include <QSet>

#include <algorithm>
#include <cassert>
#include <tuple>
#include <utility>

namespace chatterino {
namespace {
bool transitionAllowed(RumbleChannelState from, RumbleChannelState to)
{
    using S = RumbleChannelState;
    switch (from)
    {
        case S::Unresolved:
            return to == S::Offline || to == S::Connecting ||
                   to == S::Backoff || to == S::Failed || to == S::Closed;
        case S::Offline:
            return to == S::Unresolved || to == S::Closed;
        case S::Connecting:
            return to == S::Connected || to == S::Offline || to == S::Backoff ||
                   to == S::Failed || to == S::Closed;
        case S::Connected:
            return to == S::Offline || to == S::Backoff || to == S::Closed;
        case S::Backoff:
            return to == S::Unresolved || to == S::Connecting ||
                   to == S::Failed || to == S::Closed;
        case S::Failed:
            return to == S::Unresolved || to == S::Closed;
        case S::Closed:
            return false;
    }
    return false;
}
RumbleFailureCategory failureCategoryForRetry(RumbleRetryCause cause)
{
    switch (cause)
    {
        case RumbleRetryCause::ResolutionFailure:
            return RumbleFailureCategory::Resolution;
        case RumbleRetryCause::ProtocolFailure:
        case RumbleRetryCause::MissingInit:
        case RumbleRetryCause::HandoffLimit:
        case RumbleRetryCause::InvalidMetadata:
            return RumbleFailureCategory::Protocol;
        case RumbleRetryCause::SchedulerUnavailable:
        case RumbleRetryCause::DeadlineOverflow:
        case RumbleRetryCause::Cancelled:
            return RumbleFailureCategory::Internal;
        case RumbleRetryCause::TransportFailure:
        case RumbleRetryCause::StreamEnded:
        case RumbleRetryCause::Timeout:
        case RumbleRetryCause::RateLimited:
        case RumbleRetryCause::HttpFailure:
            return RumbleFailureCategory::Transport;
    }
    return RumbleFailureCategory::Internal;
}
class ScopedFlag
{
public:
    explicit ScopedFlag(bool &flag)
        : flag_(flag)
    {
        assert(!this->flag_);
        this->flag_ = true;
    }
    ~ScopedFlag()
    {
        this->flag_ = false;
    }

private:
    bool &flag_;
};

bool isSafeDiagnosticCode(const QString &value)
{
    return !value.isEmpty() && value.size() <= 64 &&
           std::ranges::all_of(value, [](QChar ch) {
               const auto code = ch.unicode();
               return (code >= 'a' && code <= 'z') ||
                      (code >= '0' && code <= '9') || code == '_';
           });
}
}  // namespace
RumbleFailure::RumbleFailure(RumbleFailureCategory category,
                             RumbleFailureCode code,
                             std::optional<RumbleOperatorText> text,
                             QString diagnosticCode)
    : category_(category)
    , code_(code)
    , text_(text)
    , diagnosticCode_(isSafeDiagnosticCode(diagnosticCode)
                          ? std::move(diagnosticCode)
                          : QString{})
{
}
RumbleFailureCategory RumbleFailure::category() const noexcept
{
    return category_;
}
RumbleFailureCode RumbleFailure::code() const noexcept
{
    return code_;
}
std::optional<RumbleOperatorText> RumbleFailure::text() const noexcept
{
    return text_;
}
const QString &RumbleFailure::diagnosticCode() const noexcept
{
    return diagnosticCode_;
}
std::optional<QString> RumbleFailure::operatorSafeText() const
{
    if (!text_)
        return std::nullopt;
    QString result;
    switch (*text_)
    {
        case RumbleOperatorText::ResolutionUnavailable:
            result = QStringLiteral("The channel is unavailable.");
            break;
        case RumbleOperatorText::RequestTimedOut:
            result = QStringLiteral("The request timed out.");
            break;
        case RumbleOperatorText::ServiceTemporarilyUnavailable:
            result = QStringLiteral("The service is temporarily unavailable.");
            break;
        case RumbleOperatorText::ServiceRejectedRequest:
            result = QStringLiteral("The service rejected the request.");
            break;
        case RumbleOperatorText::ResponseContractChanged:
            result = QStringLiteral("Rumble sent an unexpected response.");
            break;
        case RumbleOperatorText::ResponseLimitExceeded:
            result = QStringLiteral(
                "Rumble sent a response Chatterino could not process.");
            break;
        case RumbleOperatorText::ConnectionEnded:
            result = QStringLiteral("The connection ended.");
            break;
        case RumbleOperatorText::RetryLimitReached:
            result = QStringLiteral(
                "Rumble did not respond after several attempts.");
            break;
        case RumbleOperatorText::OperationCancelled:
            result = QStringLiteral("The operation was cancelled.");
            break;
        case RumbleOperatorText::InternalStateError:
            result = QStringLiteral(
                "Rumble could not complete the request. Try again.");
            break;
    }
    return result;
}
RumbleMessageId::RumbleMessageId(QString value)
    : value_(std::move(value))
{
}
Expected<RumbleMessageId, RumbleMessageIdError> RumbleMessageId::fromNormalized(
    QString value)
{
    if (value.isEmpty())
        return makeUnexpected(RumbleMessageIdError::Empty);
    if (value != value.trimmed() || std::ranges::any_of(value, [](QChar ch) {
            return ch.unicode() == 0 || ch.category() == QChar::Other_Control;
        }))
        return makeUnexpected(RumbleMessageIdError::NotNormalized);
    return RumbleMessageId(std::move(value));
}
const QString &RumbleMessageId::value() const noexcept
{
    return value_;
}
RumbleResolvedMetadata::RumbleResolvedMetadata(
    QString displayName, std::optional<RumbleChannelKey> channelSlug,
    std::optional<RumbleChannelKey> embedId,
    std::optional<RumbleChannelKey> streamId, QString streamTitle)
    : displayName_(std::move(displayName))
    , streamTitle_(std::move(streamTitle))
    , channelSlug_(std::move(channelSlug))
    , embedId_(std::move(embedId))
    , streamId_(std::move(streamId))
{
}
Expected<RumbleResolvedMetadata, RumbleMetadataError>
    RumbleResolvedMetadata::create(QString displayName,
                                   std::optional<RumbleChannelKey> channelSlug,
                                   std::optional<RumbleChannelKey> embedId,
                                   std::optional<RumbleChannelKey> streamId,
                                   QString streamTitle)
{
    if (channelSlug && channelSlug->kind() != RumbleChannelKeyKind::ChannelSlug)
        return makeUnexpected(RumbleMetadataError::WrongChannelKeyKind);
    if (embedId && embedId->kind() != RumbleChannelKeyKind::EmbedId)
        return makeUnexpected(RumbleMetadataError::WrongEmbedKeyKind);
    if (streamId && streamId->kind() != RumbleChannelKeyKind::StreamId)
        return makeUnexpected(RumbleMetadataError::WrongStreamKeyKind);
    return RumbleResolvedMetadata(std::move(displayName),
                                  std::move(channelSlug), std::move(embedId),
                                  std::move(streamId), std::move(streamTitle));
}
const QString &RumbleResolvedMetadata::displayName() const noexcept
{
    return displayName_;
}
const QString &RumbleResolvedMetadata::streamTitle() const noexcept
{
    return streamTitle_;
}
const std::optional<RumbleChannelKey> &RumbleResolvedMetadata::channelSlug()
    const noexcept
{
    return channelSlug_;
}
const std::optional<RumbleChannelKey> &RumbleResolvedMetadata::embedId()
    const noexcept
{
    return embedId_;
}
const std::optional<RumbleChannelKey> &RumbleResolvedMetadata::streamId()
    const noexcept
{
    return streamId_;
}
RumbleMessagePublication::RumbleMessagePublication(MessagePtr message,
                                                   RumbleMessageId id,
                                                   bool rant)
    : message_(std::move(message))
    , id_(std::move(id))
    , rant_(rant)
{
}
Expected<RumbleMessagePublication, RumbleMessageIdError>
    RumbleMessagePublication::fromDto(const rumble::MessageDto &message)
{
    return create(rumble::buildMessage(message), message.rant);
}
Expected<RumbleMessagePublication, RumbleMessageIdError>
    RumbleMessagePublication::create(MessagePtr message, bool rant)
{
    if (!message)
        return makeUnexpected(RumbleMessageIdError::Empty);
    auto id = RumbleMessageId::fromNormalized(message->id);
    if (!id)
        return makeUnexpected(id.error());
    return RumbleMessagePublication(std::move(message), std::move(*id), rant);
}
const MessagePtr &RumbleMessagePublication::message() const noexcept
{
    return message_;
}
const RumbleMessageId &RumbleMessagePublication::id() const noexcept
{
    return id_;
}
bool RumbleMessagePublication::isRant() const noexcept
{
    return rant_;
}
RumbleChannel::RumbleChannel(QString channelIdentity, RumbleChannelKey key,
                             std::shared_ptr<RumbleDispatcher> dispatcher,
                             std::weak_ptr<const void> providerIdentity)
    : Channel(std::move(channelIdentity), Type::Rumble)
    , ChannelChatters(static_cast<Channel &>(*this))
    , key_(std::move(key))
    , dispatcher_(std::move(dispatcher))
    , providerIdentity_(std::move(providerIdentity))
    , displayName_(key_.value())
{
    assert(dispatcher_ != nullptr);
    platform_ = QStringLiteral("rumble");
}
RumbleChannel::~RumbleChannel()
{
    closeForDestruction();
}
const RumbleChannelKey &RumbleChannel::key() const noexcept
{
    return key_;
}
const QString &RumbleChannel::getDisplayName() const
{
    return displayName_;
}
const QString &RumbleChannel::getLocalizedName() const
{
    return displayName_;
}
RumbleChannelState RumbleChannel::state() const noexcept
{
    const auto snapshot = std::atomic_load_explicit(&lifecycleSnapshot_,
                                                    std::memory_order_acquire);
    return snapshot ? snapshot->state : RumbleChannelState::Closed;
}
const std::optional<RumbleFailure> &RumbleChannel::failure() const
{
    return failure_;
}
const std::optional<RumbleResolvedMetadata> &RumbleChannel::metadata() const
{
    return metadata_;
}
const QString &RumbleChannel::streamTitle() const noexcept
{
    return streamTitle_;
}
RumbleLifecycleMetadata RumbleChannel::lifecycleMetadata() const
{
    const auto snapshot = std::atomic_load_explicit(&lifecycleSnapshot_,
                                                    std::memory_order_acquire);
    return snapshot ? snapshot->metadata : RumbleLifecycleMetadata{};
}
RumbleLifecycleSnapshot RumbleChannel::lifecycleSnapshot() const
{
    const auto snapshot = std::atomic_load_explicit(&lifecycleSnapshot_,
                                                    std::memory_order_acquire);
    return snapshot ? *snapshot
                    : RumbleLifecycleSnapshot{RumbleChannelState::Closed, {}};
}
std::optional<RumbleChannelDiagnosticData> RumbleChannel::diagnosticData(
    QDateTime nowUtc) const
{
    if (!dispatcher_ || !dispatcher_->isOwnerThread())
        return std::nullopt;
    if (!nowUtc.isValid())
        nowUtc = QDateTime::currentDateTimeUtc();
    nowUtc = nowUtc.toUTC();
    std::optional<std::int64_t> retryWaitMs;
    constexpr std::int64_t MAX_PUBLIC_WAIT_MS = 24LL * 60 * 60 * 1000;
    if (diagnosticRetryDelayMs_ && diagnosticRetryScheduledAtUtc_)
    {
        const auto elapsed = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(
                   diagnosticRetryScheduledAtUtc_->msecsTo(nowUtc)));
        retryWaitMs = std::clamp<std::int64_t>(
            *diagnosticRetryDelayMs_ - static_cast<std::int64_t>(elapsed), 0,
            MAX_PUBLIC_WAIT_MS);
    }

    RumbleChannelDiagnosticData result{
        .lifecycle = lifecycleSnapshot(),
        .locatorKind = key_.kind(),
        .retryWaitMs = retryWaitMs,
        .lastFailure = lastFailureCategory_,
        .lastFailureAtUtc = lastFailureAtUtc_,
    };
    QString streamId;
    if (metadata_ && metadata_->streamId())
    {
        result.hasDestination = true;
        streamId = metadata_->streamId()->value();
    }
    if (const auto session = session_.lock())
    {
        const auto sessionData = session->diagnosticSnapshot(streamId);
        result.sessionState = sessionData.state;
        result.sessionBlockedUntilUtc = sessionData.blockedUntilUtc;
        result.destinationDenied = sessionData.destinationDenied;
        result.sendInProgress = sessionData.sendInProgress;
    }
    return result;
}
const std::optional<RumblePinnedMessage> &RumbleChannel::pinnedMessage() const
{
    return pinnedMessage_;
}
std::optional<rumble::ModerationSnapshot> RumbleChannel::moderationSnapshot()
    const
{
    if (!moderationState_)
        return std::nullopt;
    return rumble::moderationSnapshot(*moderationState_);
}
std::size_t RumbleChannel::operationIndex(RumbleOperationKind kind) noexcept
{
    return kind == RumbleOperationKind::Resolver ? 0U : 1U;
}
std::weak_ptr<RumbleChannel> RumbleChannel::weakFromThis()
{
    return std::static_pointer_cast<RumbleChannel>(shared_from_this());
}
bool RumbleChannel::belongsToProvider(
    const std::shared_ptr<const void> &identity) const noexcept
{
    auto ours = providerIdentity_.lock();
    return ours && ours == identity;
}
bool RumbleChannel::isClosingOrClosed() const noexcept
{
    return closing_.load(std::memory_order_acquire) ||
           state() == RumbleChannelState::Closed;
}
bool RumbleChannel::isCurrent(RumbleOperationToken token) const noexcept
{
    if (isClosingOrClosed())
        return false;
    std::lock_guard lock(operationMutex_);
    if (isClosingOrClosed())
        return false;
    const auto &slot = operations_[operationIndex(token.kind)];
    return slot.generation && *slot.generation == token.generation;
}
void RumbleChannel::dispatchForOperation(
    RumbleOperationToken token, std::function<void(RumbleChannel &)> mutation)
{
    if (isClosingOrClosed())
        return;
    if (dispatcher_->isOwnerThread())
    {
        if (isCurrent(token))
            mutation(*this);
        return;
    }
    auto weak = weakFromThis();
    std::ignore = dispatcher_->dispatch(
        [weak, token, mutation = std::move(mutation)]() mutable {
            auto self = weak.lock();
            if (!self || !self->dispatcher_->isOwnerThread())
                return;
            if (self->isCurrent(token))
                mutation(*self);
        });
}
Expected<void, RumbleStateTransitionError> RumbleChannel::transitionTo(
    RumbleChannelState next, std::optional<RumbleFailure> failure)
{
    if (!dispatcher_->isOwnerThread())
        return makeUnexpected(RumbleStateTransitionError::WrongThread);
    return transitionToOwner(next, std::move(failure));
}
Expected<void, RumbleStateTransitionError> RumbleChannel::transitionToOwner(
    RumbleChannelState next, std::optional<RumbleFailure> failure)
{
    return commitLifecycleOwner(next, this->lifecycleMetadata(),
                                std::move(failure), false);
}
Expected<void, RumbleStateTransitionError> RumbleChannel::commitLifecycleOwner(
    RumbleChannelState next, RumbleLifecycleMetadata metadata,
    std::optional<RumbleFailure> failure, bool publishMetadataSignal)
{
    if (emittingStateSignals_)
        return makeUnexpected(RumbleStateTransitionError::ReentrantTransition);
    const auto previousSnapshot = std::atomic_load_explicit(
        &lifecycleSnapshot_, std::memory_order_acquire);
    const auto previous =
        previousSnapshot ? previousSnapshot->state : RumbleChannelState::Closed;
    if (closing_.load(std::memory_order_acquire))
    {
        if (next == RumbleChannelState::Closed && !failure)
        {
            closeOwner();
            return {};
        }
        return makeUnexpected(RumbleStateTransitionError::Closed);
    }
    if (previous == RumbleChannelState::Closed)
        return makeUnexpected(RumbleStateTransitionError::Closed);
    if (next == RumbleChannelState::Failed && !failure)
        return makeUnexpected(RumbleStateTransitionError::MissingFailure);
    if (next != RumbleChannelState::Failed && failure)
        return makeUnexpected(RumbleStateTransitionError::UnexpectedFailure);
    if (previous != next && !transitionAllowed(previous, next))
        return makeUnexpected(RumbleStateTransitionError::InvalidTransition);
    if (next == RumbleChannelState::Closed)
    {
        closeOwner();
        return {};
    }
    const bool metadataChanged =
        !previousSnapshot || previousSnapshot->metadata != metadata;
    const bool stateChanged = previous != next;
    const bool clearStreamTitle =
        !streamTitle_.isEmpty() && (next == RumbleChannelState::Unresolved ||
                                    next == RumbleChannelState::Offline ||
                                    next == RumbleChannelState::Failed);
    if (!metadataChanged && !stateChanged && !clearStreamTitle)
        return {};
    const bool wasLive = confirmedLive_.load(std::memory_order_acquire);
    const bool wasWritable = previous == RumbleChannelState::Connected;
    bool nowLive = wasLive;
    if (next == RumbleChannelState::Connected)
        nowLive = true;
    else if (next == RumbleChannelState::Offline)
        nowLive = false;
    const bool nowWritable = next == RumbleChannelState::Connected;
    if (stateChanged && wasWritable != nowWritable)
        sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    auto committedSnapshot = std::make_shared<const RumbleLifecycleSnapshot>(
        RumbleLifecycleSnapshot{next, std::move(metadata)});
    const auto nowUtc = QDateTime::currentDateTimeUtc();
    if (committedSnapshot->metadata.scheduledAtMs &&
        committedSnapshot->metadata.deadlineAtMs &&
        *committedSnapshot->metadata.scheduledAtMs >= 0 &&
        *committedSnapshot->metadata.deadlineAtMs >=
            *committedSnapshot->metadata.scheduledAtMs)
    {
        constexpr std::int64_t MAX_PUBLIC_WAIT_MS = 24LL * 60 * 60 * 1000;
        diagnosticRetryDelayMs_ =
            std::min(*committedSnapshot->metadata.deadlineAtMs -
                         *committedSnapshot->metadata.scheduledAtMs,
                     MAX_PUBLIC_WAIT_MS);
        diagnosticRetryScheduledAtUtc_ = nowUtc;
    }
    else
    {
        diagnosticRetryDelayMs_.reset();
        diagnosticRetryScheduledAtUtc_.reset();
    }
    if (failure)
    {
        lastFailureCategory_ = failure->category();
        lastFailureAtUtc_ = nowUtc;
    }
    else if (next == RumbleChannelState::Backoff &&
             committedSnapshot->metadata.retryCause &&
             *committedSnapshot->metadata.retryCause !=
                 RumbleRetryCause::Cancelled)
    {
        lastFailureCategory_ =
            failureCategoryForRetry(*committedSnapshot->metadata.retryCause);
        lastFailureAtUtc_ = nowUtc;
    }
    failure_ =
        next == RumbleChannelState::Failed ? std::move(failure) : std::nullopt;
    if (clearStreamTitle)
    {
        streamTitle_.clear();
        if (metadata_)
        {
            auto clearedMetadata = RumbleResolvedMetadata::create(
                metadata_->displayName(), metadata_->channelSlug(),
                metadata_->embedId(), metadata_->streamId());
            assert(clearedMetadata);
            if (clearedMetadata)
                metadata_ = std::move(*clearedMetadata);
        }
    }
    std::atomic_store_explicit(&lifecycleSnapshot_,
                               std::move(committedSnapshot),
                               std::memory_order_release);
    confirmedLive_.store(nowLive, std::memory_order_release);
    if (const auto status = rumble::captureStatus(*this, nowUtc))
        rumble::logStatus(*status, nowUtc.toMSecsSinceEpoch());
    {
        ScopedFlag emitting(emittingStateSignals_);
        if (stateChanged)
            this->stateChanged.invoke(previous, next);
        if (stateChanged && wasLive != nowLive)
            liveStatusChanged.invoke();
        if (stateChanged && wasWritable != nowWritable)
            writabilityChanged.invoke();
        if (publishMetadataSignal && metadataChanged)
            lifecycleMetadataChanged.invoke();
        if (clearStreamTitle)
            streamTitleChanged.invoke();
    }
    if (closing_.load(std::memory_order_acquire))
        closeOwner();
    return {};
}
Expected<RumbleOperationToken, RumbleChannelError>
    RumbleChannel::beginOperation(RumbleOperationKind kind)
{
    if (!dispatcher_->isOwnerThread())
        return makeUnexpected(RumbleChannelError::WrongThread);
    if (isClosingOrClosed())
        return makeUnexpected(RumbleChannelError::Closed);
    std::unique_ptr<RumbleChannelOperation> previous;
    std::uint64_t generation;
    {
        std::lock_guard lock(operationMutex_);
        if (isClosingOrClosed())
            return makeUnexpected(RumbleChannelError::Closed);
        auto &slot = operations_[operationIndex(kind)];
        generation = ++nextGeneration_;
        slot.generation = generation;
        previous = std::move(slot.handle);
    }
    if (previous)
        previous->cancel();
    if (kind == RumbleOperationKind::Connection &&
        messageLengthMax_.exchange(0, std::memory_order_acq_rel) != 0)
    {
        writabilityChanged.invoke();
    }
    if (kind == RumbleOperationKind::Connection)
        sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    return RumbleOperationToken{kind, generation};
}
Expected<void, RumbleChannelError> RumbleChannel::attachOperation(
    RumbleOperationToken token,
    std::unique_ptr<RumbleChannelOperation> operation)
{
    if (!dispatcher_->isOwnerThread())
    {
        if (operation)
        {
            operation->cancel();
            auto retained =
                std::shared_ptr<RumbleChannelOperation>(std::move(operation));
            dispatcher_->dispose([retained = std::move(retained)] {});
        }
        return makeUnexpected(RumbleChannelError::WrongThread);
    }
    if (!operation)
        return makeUnexpected(RumbleChannelError::NullHandle);
    std::optional<RumbleChannelError> error;
    {
        std::lock_guard lock(operationMutex_);
        if (isClosingOrClosed())
        {
            error = RumbleChannelError::StaleOperation;
        }
        else
        {
            auto &slot = operations_[operationIndex(token.kind)];
            if (!slot.generation || *slot.generation != token.generation)
            {
                error = RumbleChannelError::StaleOperation;
            }
            else if (slot.handle)
            {
                error = RumbleChannelError::HandleAlreadyAttached;
            }
            else
            {
                slot.handle = std::move(operation);
            }
        }
    }
    if (error)
    {
        operation->cancel();
        return makeUnexpected(*error);
    }
    return {};
}
Expected<void, RumbleChannelError> RumbleChannel::completeOperation(
    RumbleOperationToken token)
{
    if (!dispatcher_->isOwnerThread())
        return makeUnexpected(RumbleChannelError::WrongThread);
    std::unique_ptr<RumbleChannelOperation> handle;
    {
        std::lock_guard lock(operationMutex_);
        if (isClosingOrClosed())
            return makeUnexpected(RumbleChannelError::StaleOperation);
        auto &slot = operations_[operationIndex(token.kind)];
        if (!slot.generation || *slot.generation != token.generation)
            return makeUnexpected(RumbleChannelError::StaleOperation);
        slot.generation.reset();
        handle = std::move(slot.handle);
    }
    if (handle)
        handle->release();
    return {};
}
Expected<void, RumbleChannelError> RumbleChannel::setReconnectDelegate(
    ReconnectDelegate delegate)
{
    if (!dispatcher_->isOwnerThread())
    {
        if (delegate)
            dispatcher_->dispose([delegate = std::move(delegate)] {});
        return makeUnexpected(RumbleChannelError::WrongThread);
    }
    if (isClosingOrClosed())
        return makeUnexpected(RumbleChannelError::Closed);
    bool previous;
    bool current;
    ReconnectDelegate retired;
    {
        std::lock_guard lock(reconnectMutex_);
        if (isClosingOrClosed())
            return makeUnexpected(RumbleChannelError::Closed);
        previous = reconnectAvailable_.load(std::memory_order_acquire);
        retired = std::move(reconnectDelegate_);
        reconnectDelegate_ = std::move(delegate);
        current = static_cast<bool>(reconnectDelegate_);
        reconnectAvailable_.store(current, std::memory_order_release);
    }
    if (previous != current)
        reconnectAvailabilityChanged.invoke();
    retired = {};
    return {};
}
void RumbleChannel::publishMetadata(RumbleOperationToken token,
                                    RumbleResolvedMetadata metadata)
{
    dispatchForOperation(token, [metadata = std::move(metadata)](
                                    RumbleChannel &channel) mutable {
        channel.applyMetadataOwner(std::move(metadata));
    });
}
void RumbleChannel::publishBootstrap(
    RumbleOperationToken token, std::vector<RumbleMessagePublication> messages)
{
    dispatchForOperation(token, [messages = std::move(messages)](
                                    RumbleChannel &channel) mutable {
        channel.applyBootstrapOwner(std::move(messages));
    });
}
void RumbleChannel::publishRealtime(RumbleOperationToken token,
                                    RumbleMessagePublication message)
{
    dispatchForOperation(
        token, [message = std::move(message)](RumbleChannel &channel) mutable {
            channel.applyRealtimeOwner(std::move(message));
        });
}
void RumbleChannel::publishDeletion(RumbleOperationToken token,
                                    RumbleMessageId id)
{
    dispatchForOperation(token, [id = std::move(id)](RumbleChannel &channel) {
        channel.applyDeletionOwner(id);
    });
}
void RumbleChannel::publishNonRantClear(RumbleOperationToken token,
                                        RumbleNonRantClear operation)
{
    dispatchForOperation(
        token, [operation = std::move(operation)](RumbleChannel &channel) {
            channel.applyNonRantClearOwner(operation);
        });
}
void RumbleChannel::publishPinnedMessage(
    RumbleOperationToken token, std::optional<RumblePinnedMessage> pinned)
{
    dispatchForOperation(
        token, [pinned = std::move(pinned)](RumbleChannel &channel) mutable {
            channel.applyPinnedMessageOwner(std::move(pinned));
        });
}
void RumbleChannel::publishMessageLengthMax(RumbleOperationToken token,
                                            int maximum)
{
    dispatchForOperation(token, [maximum](RumbleChannel &channel) {
        const auto bounded = std::clamp(
            maximum, 0,
            static_cast<int>(rumble::SessionController::ABSOLUTE_TEXT_LIMIT));
        if (channel.messageLengthMax_.exchange(
                bounded, std::memory_order_acq_rel) != bounded)
        {
            channel.writabilityChanged.invoke();
        }
    });
}
void RumbleChannel::publishEmoteCatalog(RumbleOperationToken token,
                                        rumble::EmoteCatalog catalog)
{
    dispatchForOperation(
        token, [catalog = std::move(catalog)](RumbleChannel &channel) mutable {
            channel.applyEmoteCatalogOwner(std::move(catalog));
        });
}
void RumbleChannel::publishModeration(RumbleOperationToken token,
                                      rumble::StateOperation operation)
{
    dispatchForOperation(
        token, [operation = std::move(operation)](RumbleChannel &channel) {
            channel.applyModerationOwner(operation);
        });
}
void RumbleChannel::publishState(RumbleOperationToken token,
                                 RumbleChannelState next,
                                 std::optional<RumbleFailure> failure)
{
    dispatchForOperation(token, [next, failure = std::move(failure)](
                                    RumbleChannel &channel) mutable {
        std::ignore = channel.transitionToOwner(next, std::move(failure));
    });
}
void RumbleChannel::publishLifecycle(RumbleOperationToken token,
                                     RumbleChannelState next,
                                     RumbleLifecycleMetadata metadata,
                                     std::optional<RumbleFailure> failure)
{
    dispatchForOperation(
        token, [next, metadata = std::move(metadata),
                failure = std::move(failure)](RumbleChannel &channel) mutable {
            channel.applyLifecycleOwner(next, std::move(metadata),
                                        std::move(failure));
        });
}
void RumbleChannel::applyMetadataOwner(RumbleResolvedMetadata metadata)
{
    const bool displayChanged = displayName_ != metadata.displayName();
    const bool titleChanged = streamTitle_ != metadata.streamTitle();
    const bool locatorsChanged =
        !metadata_ || metadata_->channelSlug() != metadata.channelSlug() ||
        metadata_->embedId() != metadata.embedId() ||
        metadata_->streamId() != metadata.streamId();
    displayName_ = metadata.displayName();
    streamTitle_ = metadata.streamTitle();
    metadata_ = std::move(metadata);
    if (displayChanged)
        displayNameChanged.invoke();
    if (locatorsChanged)
    {
        sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
        locatorChanged.invoke();
        writabilityChanged.invoke();
    }
    refreshEmoteEligibility();
    if (titleChanged)
        streamTitleChanged.invoke();
}
void RumbleChannel::applyBootstrapOwner(
    std::vector<RumbleMessagePublication> publications)
{
    std::vector<MessagePtr> messages;
    messages.reserve(publications.size());
    for (const auto &publication : publications)
    {
        const auto &id = publication.id().value();
        rantByMessageId_.insert(id, publication.isRant());
        if (optimisticMessageIds_.remove(id) != 0)
        {
            if (const auto existing = findMessageByID(id))
            {
                replaceMessage(existing, publication.message());
                continue;
            }
        }
        messages.emplace_back(publication.message());
    }
    // Reconnect init batches can overlap both previous init history and
    // realtime appends. Use the production chronological, ID-deduplicating
    // history path rather than blindly prepending.
    fillInMissingMessages(messages);
    pruneTrackedMessages();
}
void RumbleChannel::applyRealtimeOwner(RumbleMessagePublication publication)
{
    const auto &id = publication.id().value();
    rantByMessageId_.insert(id, publication.isRant());
    if (optimisticMessageIds_.remove(id) != 0)
    {
        // A confirmed send publishes an optimistic message with the provider
        // ID. Replace it atomically when the authoritative realtime event
        // arrives so metadata is upgraded without a duplicate or a transient
        // non-Rumble username presentation.
        if (const auto existing = findMessageByID(id))
        {
            replaceMessage(existing, publication.message());
        }
        else
        {
            addMessage(publication.message(), MessageContext::Original);
        }
    }
    else if (!findMessageByID(id))
    {
        addMessage(publication.message(), MessageContext::Original);
    }
    pruneTrackedMessages();
}
void RumbleChannel::applyDeletionOwner(const RumbleMessageId &id)
{
    disableMessage(id.value());
}
void RumbleChannel::applyNonRantClearOwner(const RumbleNonRantClear &operation)
{
    const auto disableIfNonRant = [this](const RumbleMessageId &id) {
        auto tracked = rantByMessageId_.constFind(id.value());
        if (tracked != rantByMessageId_.cend() && !tracked.value())
            disableMessage(id.value());
    };
    if (operation.mode == RumbleNonRantClearMode::ListedIds)
    {
        for (const auto &id : operation.ids)
            disableIfNonRant(id);
        return;
    }
    for (const auto &message : getMessageSnapshot())
    {
        auto tracked = rantByMessageId_.constFind(message->id);
        if (tracked != rantByMessageId_.cend() && !tracked.value())
            message->flags.set(MessageFlag::Disabled);
    }
}
void RumbleChannel::applyPinnedMessageOwner(
    std::optional<RumblePinnedMessage> pinned)
{
    if (pinnedMessage_ == pinned)
        return;
    pinnedMessage_ = std::move(pinned);
    pinnedMessageChanged.invoke();
}
void RumbleChannel::applyEmoteCatalogOwner(rumble::EmoteCatalog catalog)
{
    this->emoteDefinitions_ = std::move(catalog.emotes);
    this->completionEmotes_.clear();
    this->completionEmotes_.reserve(this->emoteDefinitions_.size());
    for (const auto &definition : this->emoteDefinitions_)
    {
        this->completionEmotes_.push_back({
            .emote = rumble::makeEmote(definition),
            .searchName = definition.name,
            .insertionText = definition.insertionText(),
            .scope = definition.scope,
            .subscribersOnly = definition.subscribersOnly,
        });
    }
    this->writabilityChanged.invoke();
}
void RumbleChannel::applyModerationOwner(
    const rumble::StateOperation &operation)
{
    QString channelValue = key_.value();
    if (metadata_ && metadata_->streamId())
        channelValue = metadata_->streamId()->value();
    auto channelId = rumble::ModerationIdentity::fromProvider(channelValue);
    if (!channelId)
        return;
    std::optional<rumble::ModerationIdentity> accountId;
    if (const auto session = session_.lock();
        session && !session->accountId().isEmpty())
    {
        auto parsed =
            rumble::ModerationIdentity::fromProvider(session->accountId());
        if (parsed)
            accountId = std::move(*parsed);
    }
    rumble::ModerationScope scope{std::move(accountId), std::move(*channelId)};
    if (!moderationState_ || moderationState_->scope() != scope)
    {
        moderationState_.emplace(scope);
        moderationRevision_ = 0;
    }
    auto event = rumble::moderationEventFromStateOperation(
        operation, scope, moderationRevision_++);
    if (event && moderationState_->apply(*event) ==
                     rumble::ModerationApplyResult::Applied)
        moderationStateChanged.invoke();
}
void RumbleChannel::applyLifecycleOwner(RumbleChannelState next,
                                        RumbleLifecycleMetadata metadata,
                                        std::optional<RumbleFailure> failure)
{
    std::ignore = commitLifecycleOwner(next, std::move(metadata),
                                       std::move(failure), true);
}
void RumbleChannel::close()
{
    if (closing_.exchange(true, std::memory_order_acq_rel))
        return;
    // Operation handles are transport-owned and require thread-safe
    // cancellation. Cancel immediately so an unavailable UI owner cannot
    // leave network work alive indefinitely.
    cancelOperations();
    if (dispatcher_->isOwnerThread())
    {
        if (!emittingStateSignals_)
            closeOwner();
        // A signal callback's close is completed by transitionToOwner after
        // the current state/live pair has finished.
        return;
    }
    auto weak = weakFromThis();
    const bool accepted = dispatcher_->dispatch([weak] {
        auto self = weak.lock();
        if (!self || !self->dispatcher_->isOwnerThread())
            return;
        self->closeOwner();
    });
    if (!accepted)
        closeWithoutOwner();
}
RumbleChannel::ReconnectDelegate RumbleChannel::takeReconnectDelegate(
    bool &wasAvailable) noexcept
{
    std::lock_guard lock(reconnectMutex_);
    wasAvailable =
        reconnectAvailable_.exchange(false, std::memory_order_acq_rel);
    return std::move(reconnectDelegate_);
}
bool RumbleChannel::clearReconnectDelegate() noexcept
{
    bool wasAvailable = false;
    auto retired = takeReconnectDelegate(wasAvailable);
    retired = {};
    return wasAvailable;
}
void RumbleChannel::cancelOperations() noexcept
{
    using Handles = std::array<std::unique_ptr<RumbleChannelOperation>, 2>;
    Handles handles;
    {
        std::lock_guard lock(operationMutex_);
        ++nextGeneration_;
        for (std::size_t index = 0; index < operations_.size(); ++index)
        {
            operations_[index].generation.reset();
            handles[index] = std::move(operations_[index].handle);
        }
    }
    bool hasHandle = false;
    for (auto &handle : handles)
    {
        if (handle)
        {
            hasHandle = true;
            handle->cancel();
        }
    }
    if (hasHandle && dispatcher_ && !dispatcher_->isOwnerThread())
    {
        auto retained = std::make_shared<Handles>(std::move(handles));
        dispatcher_->dispose([retained = std::move(retained)] {});
    }
}
void RumbleChannel::closeOwner()
{
    closing_.store(true, std::memory_order_release);
    cancelOperations();
    const auto previous = state();
    if (previous == RumbleChannelState::Closed)
    {
        std::ignore = clearReconnectDelegate();
        return;
    }
    const bool wasLive =
        confirmedLive_.exchange(false, std::memory_order_acq_rel);
    const bool wasWritable = previous == RumbleChannelState::Connected;
    sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_store_explicit(&lifecycleSnapshot_, closedLifecycleSnapshot_,
                               std::memory_order_release);
    failure_.reset();
    diagnosticRetryDelayMs_.reset();
    diagnosticRetryScheduledAtUtc_.reset();
    const auto nowUtc = QDateTime::currentDateTimeUtc();
    if (const auto status = rumble::captureStatus(*this, nowUtc))
        rumble::logStatus(*status, nowUtc.toMSecsSinceEpoch());
    const bool couldReconnect = clearReconnectDelegate();
    ScopedFlag emitting(emittingStateSignals_);
    stateChanged.invoke(previous, RumbleChannelState::Closed);
    if (wasLive)
        liveStatusChanged.invoke();
    if (wasWritable)
        writabilityChanged.invoke();
    if (couldReconnect)
        reconnectAvailabilityChanged.invoke();
}
void RumbleChannel::closeWithoutOwner() noexcept
{
    closing_.store(true, std::memory_order_release);
    confirmedLive_.store(false, std::memory_order_release);
    sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    cancelOperations();
    std::atomic_store_explicit(&lifecycleSnapshot_, closedLifecycleSnapshot_,
                               std::memory_order_release);
    bool couldReconnect = false;
    auto retired = takeReconnectDelegate(couldReconnect);
    (void)couldReconnect;
    if (retired)
    {
        dispatcher_->dispose([retired = std::move(retired)] {});
    }
}
void RumbleChannel::closeForDestruction() noexcept
{
    closing_.store(true, std::memory_order_release);
    confirmedLive_.store(false, std::memory_order_release);
    sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_store_explicit(&lifecycleSnapshot_, closedLifecycleSnapshot_,
                               std::memory_order_release);
    std::ignore = clearReconnectDelegate();
    cancelOperations();
}
void RumbleChannel::gateForDeferredDestruction() noexcept
{
    closing_.store(true, std::memory_order_release);
    confirmedLive_.store(false, std::memory_order_release);
    sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_store_explicit(&lifecycleSnapshot_, closedLifecycleSnapshot_,
                               std::memory_order_release);
    cancelOperations();
}
bool RumbleChannel::canSendMessage() const
{
    const auto session = session_.lock();
    const auto stream = getCurrentStreamID();
    return state() == RumbleChannelState::Connected && session &&
           !stream.isEmpty() && session->isWritable(stream);
}
bool RumbleChannel::isWritable() const
{
    return canSendMessage();
}
MessageSendContext RumbleChannel::messageSendContext() const
{
    auto context = Channel::messageSendContext();
    context.platform = QStringLiteral("rumble");
    context.channelID = getName();
    const auto session = session_.lock();
    context.writable = isWritable();
    context.authenticated =
        session && session->state() == rumble::SessionState::Valid;
    context.accountID = session ? session->accountId() : QString{};
    const auto observed = messageLengthMax_.load(std::memory_order_acquire);
    context.maxMessageLength =
        observed > 0 ? observed
                     : rumble::SessionController::ABSOLUTE_TEXT_LIMIT;
    for (const auto &entry : this->availableEmotes())
    {
        const auto availability = DraftEmoteAvailability{
            .platform = QStringLiteral("rumble"),
            .channelID = entry.scope == rumble::EmoteScope::Channel
                             ? std::optional<QString>{context.channelID}
                             : std::nullopt,
            .accountID = context.accountID,
        };
        context.emoteCapabilities.push_back({
            .identity =
                {
                    .provider = QStringLiteral("rumble"),
                    .id = entry.emote->id,
                },
            .insertionText = entry.insertionText,
            .availability = availability,
        });
    }
    context.emoteCapabilitiesComplete = true;
    return context;
}
std::vector<RumbleCompletionEmote> RumbleChannel::availableEmotes() const
{
    std::vector<RumbleCompletionEmote> available;
    const auto session = session_.lock();
    const auto stream = getCurrentStreamID();
    if (!isLive() || !session || stream.isEmpty() ||
        session->state() != rumble::SessionState::Valid)
    {
        return available;
    }
    const auto eligibility = session->emoteEligibility(stream);
    available.reserve(this->completionEmotes_.size());
    for (const auto &entry : this->completionEmotes_)
    {
        if (entry.scope == rumble::EmoteScope::Global && !entry.subscribersOnly)
        {
            available.push_back(entry);
            continue;
        }
        if (!eligibility)
        {
            continue;
        }
        if (entry.subscribersOnly)
        {
            if (eligibility->subscriberOrAdmin)
            {
                available.push_back(entry);
            }
            continue;
        }
        if (eligibility->following || eligibility->subscriberOrAdmin)
        {
            available.push_back(entry);
        }
    }
    return available;
}
void RumbleChannel::refreshEmoteEligibility()
{
    const auto session = session_.lock();
    const auto stream = getCurrentStreamID();
    if (session && !stream.isEmpty())
    {
        session->ensureEmoteEligibility(stream);
    }
}
void RumbleChannel::sendMessage(const QString &message)
{
    sendMessageAsync(message, [weak = weakFromThis()](SendResult result) {
        if (result.outcome != SendOutcome::Confirmed)
        {
            if (const auto channel = weak.lock();
                channel && !result.userMessage.isEmpty())
            {
                channel->addSystemMessage(result.userMessage);
            }
        }
    });
}
void RumbleChannel::sendMessageAsync(QString message, SendCallback callback)
{
    if (state() != RumbleChannelState::Connected)
    {
        if (callback)
        {
            callback({SendOutcome::DefiniteFailure,
                      QStringLiteral(
                          "Rumble chat is unavailable while the stream is "
                          "offline or reconnecting.")});
        }
        return;
    }
    const auto session = session_.lock();
    const auto stream = getCurrentStreamID();
    if (!session || stream.isEmpty())
    {
        if (callback)
            callback(
                {SendOutcome::Unsupported,
                 QStringLiteral("Sign in to Rumble before sending.")});
        return;
    }
    const auto maximum = messageSendContext().maxMessageLength;
    if (maximum > 0 && message.size() > maximum)
    {
        if (callback)
            callback(
                {SendOutcome::DefiniteFailure,
                 QStringLiteral("This message is too long for Rumble.")});
        return;
    }
    auto identity = session->identity();
    if (!identity && !session->accountId().isEmpty())
    {
        identity = rumble::SessionIdentity{
            .userID = session->accountId(),
        };
    }
    const auto optimisticText = message;
    const auto weak = weakFromThis();
    const auto generation = sendGeneration_.load(std::memory_order_acquire);
    session->send(
        stream, std::move(message),
        [weak, stream, generation, optimisticText = std::move(optimisticText),
         identity = std::move(identity),
         callback = std::move(callback)](rumble::SendResult result) mutable {
            const auto channel = weak.lock();
            if (!channel ||
                channel->sendGeneration_.load(std::memory_order_acquire) !=
                    generation ||
                channel->getCurrentStreamID() != stream)
                return;

            auto deliver = [weak, stream, generation,
                            optimisticText = std::move(optimisticText),
                            identity = std::move(identity),
                            callback = std::move(callback),
                            result = std::move(result)]() mutable {
                const auto owner = weak.lock();
                if (!owner || !owner->dispatcher_->isOwnerThread() ||
                    owner->sendGeneration_.load(std::memory_order_acquire) !=
                        generation ||
                    owner->getCurrentStreamID() != stream)
                    return;

                if (result.outcome == rumble::SendOutcome::Confirmed &&
                    result.messageId && identity &&
                    !owner->findMessageByID(*result.messageId))
                {
                    const auto now = QDateTime::currentDateTimeUtc();
                    rumble::MessageDto dto{
                        .id = *result.messageId,
                        .userId = identity->userID,
                        .channelId = stream,
                        .text = optimisticText,
                        .createdOn = now.toString(Qt::ISODateWithMs),
                        .timestamp = now,
                        .loginName =
                            identity->username.isEmpty()
                                ? std::nullopt
                                : std::optional<QString>{identity->username},
                        .displayName =
                            identity->username.isEmpty()
                                ? std::nullopt
                                : std::optional<QString>{identity->username},
                        .channelName = owner->getDisplayName(),
                        .badgeIds = QStringList{},
                        .roleIds = QStringList{},
                        .source = QString{},
                    };
                    dto.resolvedEmotes = rumble::resolveEmoteOccurrences(
                        dto.text, {.emotes = owner->emoteDefinitions_});
                    auto optimistic = rumble::buildMessage(dto);
                    optimistic->flags.set(
                        MessageFlag::DoNotTriggerNotification);
                    auto publication = RumbleMessagePublication::create(
                        std::move(optimistic), false);
                    if (publication)
                    {
                        owner->rantByMessageId_.insert(
                            publication->id().value(), false);
                        owner->optimisticMessageIds_.insert(
                            publication->id().value());
                        owner->addMessage(publication->message(),
                                          MessageContext::Original);
                        owner->pruneTrackedMessages();
                    }
                }

                if (!callback)
                    return;
                SendOutcome outcome = SendOutcome::DefiniteFailure;
                switch (result.outcome)
                {
                    case rumble::SendOutcome::Confirmed:
                        outcome = SendOutcome::Confirmed;
                        break;
                    case rumble::SendOutcome::DefiniteFailure:
                        outcome = SendOutcome::DefiniteFailure;
                        break;
                    case rumble::SendOutcome::Ambiguous:
                        outcome = SendOutcome::Ambiguous;
                        break;
                    case rumble::SendOutcome::Cancelled:
                        outcome = SendOutcome::Cancelled;
                        break;
                }
                callback({outcome, std::move(result.userMessage)});
            };

            if (channel->dispatcher_->isOwnerThread())
            {
                deliver();
            }
            else
            {
                std::ignore =
                    channel->dispatcher_->dispatch(std::move(deliver));
            }
        });
}
void RumbleChannel::setSessionController(
    std::weak_ptr<rumble::SessionController> session)
{
    session_ = std::move(session);
    sendGeneration_.fetch_add(1, std::memory_order_acq_rel);
    refreshEmoteEligibility();
}
bool RumbleChannel::isLive() const
{
    return confirmedLive_.load(std::memory_order_acquire);
}
bool RumbleChannel::canReconnect() const
{
    return state() != RumbleChannelState::Closed &&
           reconnectAvailable_.load(std::memory_order_acquire);
}
void RumbleChannel::reconnect()
{
    if (isClosingOrClosed())
        return;
    if (dispatcher_->isOwnerThread())
    {
        requestReconnectOwner();
        return;
    }
    auto weak = weakFromThis();
    std::ignore = dispatcher_->dispatch([weak] {
        if (auto self = weak.lock())
        {
            if (!self->dispatcher_->isOwnerThread())
                return;
            self->requestReconnectOwner();
        }
    });
}
void RumbleChannel::requestReconnectOwner()
{
    if (isClosingOrClosed())
        return;
    ReconnectDelegate delegate;
    {
        std::lock_guard lock(reconnectMutex_);
        if (isClosingOrClosed())
            return;
        delegate = reconnectDelegate_;
    }
    if (delegate)
        delegate();
}
QString RumbleChannel::getCurrentStreamID() const
{
    if (metadata_ && metadata_->streamId())
        return metadata_->streamId()->value();
    return {};
}
void RumbleChannel::messageRemovedFromStart(const MessagePtr &message)
{
    if (message)
    {
        rantByMessageId_.remove(message->id);
        optimisticMessageIds_.remove(message->id);
    }
}
void RumbleChannel::pruneTrackedMessages()
{
    QSet<QString> liveIds;
    const auto snapshot = getMessageSnapshot();
    liveIds.reserve(static_cast<qsizetype>(snapshot.size()));
    for (const auto &message : snapshot)
        liveIds.insert(message->id);
    for (auto it = rantByMessageId_.begin(); it != rantByMessageId_.end();)
    {
        if (!liveIds.contains(it.key()))
            it = rantByMessageId_.erase(it);
        else
            ++it;
    }
    for (auto it = optimisticMessageIds_.begin();
         it != optimisticMessageIds_.end();)
    {
        if (!liveIds.contains(*it))
            it = optimisticMessageIds_.erase(it);
        else
            ++it;
    }
}
}  // namespace chatterino
