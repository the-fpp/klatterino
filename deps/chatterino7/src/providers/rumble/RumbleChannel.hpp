// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#include "common/Channel.hpp"
#include "common/ChannelChatters.hpp"
#include "providers/rumble/RumbleChannelKey.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/rumble/RumbleEmotes.hpp"
#include "providers/rumble/RumbleModeration.hpp"
#include "util/Expected.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace chatterino {
namespace rumble {
struct MessageDto;
class SessionController;
enum class SessionState : std::uint8_t;
}  // namespace rumble

enum class RumbleChannelState : std::uint8_t {
    Unresolved,
    Offline,
    Connecting,
    Connected,
    Backoff,
    Failed,
    Closed,
};

enum class RumbleRetryCause : std::uint8_t {
    ResolutionFailure,
    TransportFailure,
    StreamEnded,
    Timeout,
    RateLimited,
    HttpFailure,
    ProtocolFailure,
    MissingInit,
    HandoffLimit,
    SchedulerUnavailable,
    DeadlineOverflow,
    InvalidMetadata,
    Cancelled,
};

struct RumbleLifecycleMetadata {
    std::uint32_t consecutiveFailures = 0;
    std::optional<std::int64_t> scheduledAtMs;
    std::optional<std::int64_t> deadlineAtMs;
    bool rateLimited = false;
    std::optional<RumbleRetryCause> retryCause;
    friend bool operator==(const RumbleLifecycleMetadata &,
                           const RumbleLifecycleMetadata &) = default;
};

struct RumbleLifecycleSnapshot {
    RumbleChannelState state = RumbleChannelState::Unresolved;
    RumbleLifecycleMetadata metadata;
    friend bool operator==(const RumbleLifecycleSnapshot &,
                           const RumbleLifecycleSnapshot &) = default;
};
enum class RumbleFailureCategory : std::uint8_t {
    Resolution,
    Transport,
    Protocol,
    Authentication,
    Internal,
};
struct RumbleChannelDiagnosticData {
    RumbleLifecycleSnapshot lifecycle;
    RumbleChannelKeyKind locatorKind = RumbleChannelKeyKind::ChannelSlug;
    std::optional<rumble::SessionState> sessionState;
    std::optional<QDateTime> sessionBlockedUntilUtc;
    bool destinationDenied = false;
    bool sendInProgress = false;
    bool hasDestination = false;
    std::optional<std::int64_t> retryWaitMs;
    std::optional<RumbleFailureCategory> lastFailure;
    std::optional<QDateTime> lastFailureAtUtc;
};
enum class RumbleFailureCode : std::uint8_t {
    Unavailable,
    Timeout,
    RetryableHttp,
    RejectedHttp,
    MalformedResponse,
    StreamEnded,
    RetryExhausted,
    Cancelled,
    InvariantViolation,
};
enum class RumbleOperatorText : std::uint8_t {
    ResolutionUnavailable,
    RequestTimedOut,
    ServiceTemporarilyUnavailable,
    ServiceRejectedRequest,
    ResponseContractChanged,
    ResponseLimitExceeded,
    ConnectionEnded,
    RetryLimitReached,
    OperationCancelled,
    InternalStateError,
};
class RumbleFailure
{
public:
    RumbleFailure(RumbleFailureCategory category, RumbleFailureCode code,
                  std::optional<RumbleOperatorText> text = std::nullopt,
                  QString diagnosticCode = {});
    RumbleFailureCategory category() const noexcept;
    RumbleFailureCode code() const noexcept;
    std::optional<RumbleOperatorText> text() const noexcept;
    const QString &diagnosticCode() const noexcept;
    std::optional<QString> operatorSafeText() const;
    friend bool operator==(const RumbleFailure &,
                           const RumbleFailure &) = default;

private:
    RumbleFailureCategory category_;
    RumbleFailureCode code_;
    std::optional<RumbleOperatorText> text_;
    QString diagnosticCode_;
};
enum class RumbleMessageIdError : std::uint8_t { Empty, NotNormalized };
class RumbleMessageId
{
public:
    static Expected<RumbleMessageId, RumbleMessageIdError> fromNormalized(
        QString value);
    const QString &value() const noexcept;
    friend bool operator==(const RumbleMessageId &,
                           const RumbleMessageId &) = default;

private:
    explicit RumbleMessageId(QString value);
    QString value_;
};
enum class RumbleMetadataError : std::uint8_t {
    WrongChannelKeyKind,
    WrongEmbedKeyKind,
    WrongStreamKeyKind,
};
class RumbleResolvedMetadata
{
public:
    static Expected<RumbleResolvedMetadata, RumbleMetadataError> create(
        QString displayName,
        std::optional<RumbleChannelKey> channelSlug = std::nullopt,
        std::optional<RumbleChannelKey> embedId = std::nullopt,
        std::optional<RumbleChannelKey> streamId = std::nullopt,
        QString streamTitle = {});
    const QString &displayName() const noexcept;
    const QString &streamTitle() const noexcept;
    const std::optional<RumbleChannelKey> &channelSlug() const noexcept;
    const std::optional<RumbleChannelKey> &embedId() const noexcept;
    const std::optional<RumbleChannelKey> &streamId() const noexcept;
    friend bool operator==(const RumbleResolvedMetadata &,
                           const RumbleResolvedMetadata &) = default;

private:
    RumbleResolvedMetadata(QString displayName,
                           std::optional<RumbleChannelKey> channelSlug,
                           std::optional<RumbleChannelKey> embedId,
                           std::optional<RumbleChannelKey> streamId,
                           QString streamTitle);
    QString displayName_;
    QString streamTitle_;
    std::optional<RumbleChannelKey> channelSlug_;
    std::optional<RumbleChannelKey> embedId_;
    std::optional<RumbleChannelKey> streamId_;
};
class RumbleMessagePublication
{
public:
    static Expected<RumbleMessagePublication, RumbleMessageIdError> fromDto(
        const rumble::MessageDto &message);
    static Expected<RumbleMessagePublication, RumbleMessageIdError> create(
        MessagePtr message, bool rant);
    const MessagePtr &message() const noexcept;
    const RumbleMessageId &id() const noexcept;
    bool isRant() const noexcept;
    friend bool operator==(const RumbleMessagePublication &,
                           const RumbleMessagePublication &) = default;

private:
    RumbleMessagePublication(MessagePtr message, RumbleMessageId id, bool rant);
    MessagePtr message_;
    RumbleMessageId id_;
    bool rant_;
};
enum class RumbleNonRantClearMode : std::uint8_t { ListedIds, AllKnown };
struct RumbleNonRantClear {
    RumbleNonRantClearMode mode = RumbleNonRantClearMode::AllKnown;
    std::vector<RumbleMessageId> ids;
    friend bool operator==(const RumbleNonRantClear &,
                           const RumbleNonRantClear &) = default;
};
struct RumblePinnedMessage {
    RumbleMessagePublication publication;
    friend bool operator==(const RumblePinnedMessage &,
                           const RumblePinnedMessage &) = default;
};
struct RumbleCompletionEmote {
    EmotePtr emote;
    QString searchName;
    QString insertionText;
    rumble::EmoteScope scope = rumble::EmoteScope::Global;
    bool subscribersOnly = false;

    friend bool operator==(const RumbleCompletionEmote &,
                           const RumbleCompletionEmote &) = default;
};
enum class RumbleStateTransitionError : std::uint8_t {
    WrongThread,
    ReentrantTransition,
    Closed,
    InvalidTransition,
    MissingFailure,
    UnexpectedFailure,
};
enum class RumbleChannelError : std::uint8_t {
    WrongThread,
    Closed,
    NullHandle,
    StaleOperation,
    HandleAlreadyAttached,
};
enum class RumbleOperationKind : std::uint8_t { Resolver, Connection };
struct RumbleOperationToken {
    RumbleOperationKind kind;
    std::uint64_t generation;
    friend bool operator==(const RumbleOperationToken &,
                           const RumbleOperationToken &) = default;
};
class RumbleChannelOperation
{
public:
    virtual ~RumbleChannelOperation() = default;
    virtual void cancel() noexcept = 0;
    // Mark a successfully completed operation inactive without reporting it
    // as cancellation. Implementations must make release/cancel idempotent.
    virtual void release() noexcept = 0;
};
class RumbleChannel : public Channel, public ChannelChatters
{
public:
    using ReconnectDelegate = std::function<void()>;
    ~RumbleChannel() override;
    const RumbleChannelKey &key() const noexcept;
    const QString &getDisplayName() const override;
    const QString &getLocalizedName() const override;
    RumbleChannelState state() const noexcept;
    const std::optional<RumbleFailure> &failure() const;
    const std::optional<RumbleResolvedMetadata> &metadata() const;
    const QString &streamTitle() const noexcept;
    RumbleLifecycleMetadata lifecycleMetadata() const;
    RumbleLifecycleSnapshot lifecycleSnapshot() const;
    /// Returns one owner-thread-only, identity-free copy. Off-owner access
    /// fails closed instead of reading owner-affine channel/session fields.
    std::optional<RumbleChannelDiagnosticData> diagnosticData(
        QDateTime nowUtc) const;
    const std::optional<RumblePinnedMessage> &pinnedMessage() const;
    std::optional<rumble::ModerationSnapshot> moderationSnapshot() const;
    pajlada::Signals::Signal<RumbleChannelState, RumbleChannelState>
        stateChanged;
    pajlada::Signals::NoArgSignal locatorChanged;
    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal reconnectAvailabilityChanged;
    pajlada::Signals::NoArgSignal pinnedMessageChanged;
    pajlada::Signals::NoArgSignal lifecycleMetadataChanged;
    /// Current live video title for split-header presentation. This is kept
    /// separate from displayNameChanged so tab identity remains stable.
    pajlada::Signals::NoArgSignal streamTitleChanged;
    pajlada::Signals::NoArgSignal writabilityChanged;
    pajlada::Signals::NoArgSignal moderationStateChanged;
    Expected<void, RumbleStateTransitionError> transitionTo(
        RumbleChannelState next,
        std::optional<RumbleFailure> failure = std::nullopt);
    Expected<RumbleOperationToken, RumbleChannelError> beginOperation(
        RumbleOperationKind kind);
    Expected<void, RumbleChannelError> attachOperation(
        RumbleOperationToken token,
        std::unique_ptr<RumbleChannelOperation> operation);
    Expected<void, RumbleChannelError> completeOperation(
        RumbleOperationToken token);
    Expected<void, RumbleChannelError> setReconnectDelegate(
        ReconnectDelegate delegate);
    void publishMetadata(RumbleOperationToken, RumbleResolvedMetadata);
    void publishBootstrap(RumbleOperationToken,
                          std::vector<RumbleMessagePublication>);
    void publishRealtime(RumbleOperationToken, RumbleMessagePublication);
    void publishDeletion(RumbleOperationToken, RumbleMessageId);
    void publishNonRantClear(RumbleOperationToken, RumbleNonRantClear);
    void publishPinnedMessage(RumbleOperationToken,
                              std::optional<RumblePinnedMessage>);
    void publishMessageLengthMax(RumbleOperationToken, int maximum);
    void publishEmoteCatalog(RumbleOperationToken, rumble::EmoteCatalog);
    void publishModeration(RumbleOperationToken, rumble::StateOperation);
    void publishState(RumbleOperationToken, RumbleChannelState,
                      std::optional<RumbleFailure> = std::nullopt);
    void publishLifecycle(RumbleOperationToken, RumbleChannelState,
                          RumbleLifecycleMetadata,
                          std::optional<RumbleFailure> = std::nullopt);
    void close();
    bool canSendMessage() const override;
    bool isWritable() const override;
    MessageSendContext messageSendContext() const override;
    std::vector<RumbleCompletionEmote> availableEmotes() const;
    void refreshEmoteEligibility();
    void sendMessage(const QString &message) override;
    void sendMessageAsync(QString message, SendCallback callback) override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;
    void setSessionController(std::weak_ptr<rumble::SessionController> session);

protected:
    void messageRemovedFromStart(const MessagePtr &) override;

private:
    RumbleChannel(QString channelIdentity, RumbleChannelKey key,
                  std::shared_ptr<RumbleDispatcher> dispatcher,
                  std::weak_ptr<const void> providerIdentity);

    struct OperationSlot {
        std::optional<std::uint64_t> generation;
        std::unique_ptr<RumbleChannelOperation> handle;
    };
    static std::size_t operationIndex(RumbleOperationKind) noexcept;
    std::weak_ptr<RumbleChannel> weakFromThis();
    bool belongsToProvider(const std::shared_ptr<const void> &) const noexcept;
    bool isClosingOrClosed() const noexcept;
    bool isCurrent(RumbleOperationToken) const noexcept;
    void dispatchForOperation(RumbleOperationToken,
                              std::function<void(RumbleChannel &)>);
    Expected<void, RumbleStateTransitionError> transitionToOwner(
        RumbleChannelState, std::optional<RumbleFailure>);
    Expected<void, RumbleStateTransitionError> commitLifecycleOwner(
        RumbleChannelState, RumbleLifecycleMetadata,
        std::optional<RumbleFailure>, bool);
    void applyMetadataOwner(RumbleResolvedMetadata);
    void applyBootstrapOwner(std::vector<RumbleMessagePublication>);
    void applyRealtimeOwner(RumbleMessagePublication);
    void applyDeletionOwner(const RumbleMessageId &);
    void applyNonRantClearOwner(const RumbleNonRantClear &);
    void applyPinnedMessageOwner(std::optional<RumblePinnedMessage>);
    void applyEmoteCatalogOwner(rumble::EmoteCatalog);
    void applyModerationOwner(const rumble::StateOperation &);
    void applyLifecycleOwner(RumbleChannelState, RumbleLifecycleMetadata,
                             std::optional<RumbleFailure>);
    void requestReconnectOwner();
    ReconnectDelegate takeReconnectDelegate(bool &wasAvailable) noexcept;
    bool clearReconnectDelegate() noexcept;
    void cancelOperations() noexcept;
    void closeOwner();
    void closeWithoutOwner() noexcept;
    void gateForDeferredDestruction() noexcept;
    void closeForDestruction() noexcept;
    void pruneTrackedMessages();
    RumbleChannelKey key_;
    std::shared_ptr<RumbleDispatcher> dispatcher_;
    std::weak_ptr<const void> providerIdentity_;
    QString displayName_;
    QString streamTitle_;
    std::optional<RumbleResolvedMetadata> metadata_;
    std::optional<RumbleFailure> failure_;
    std::optional<RumbleFailureCategory> lastFailureCategory_;
    std::optional<QDateTime> lastFailureAtUtc_;
    std::optional<std::int64_t> diagnosticRetryDelayMs_;
    std::optional<QDateTime> diagnosticRetryScheduledAtUtc_;
    std::optional<RumblePinnedMessage> pinnedMessage_;
    std::vector<rumble::EmoteDefinition> emoteDefinitions_;
    // Provider-confirmed availability is deliberately independent from the
    // SSE transport state. A recoverable disconnect/backoff must not make a
    // live stream flash offline; only a confirmed Offline transition clears
    // this value.
    std::atomic<bool> confirmedLive_{false};
    std::vector<RumbleCompletionEmote> completionEmotes_;
    std::shared_ptr<const RumbleLifecycleSnapshot> lifecycleSnapshot_ =
        std::make_shared<const RumbleLifecycleSnapshot>();
    const std::shared_ptr<const RumbleLifecycleSnapshot>
        closedLifecycleSnapshot_ =
            std::make_shared<const RumbleLifecycleSnapshot>(
                RumbleLifecycleSnapshot{RumbleChannelState::Closed, {}});
    QHash<QString, bool> rantByMessageId_;
    QSet<QString> optimisticMessageIds_;
    // State/live signal pairs are atomic from observers' perspective. A typed
    // transition is rejected while either signal is being delivered; void
    // close requests are deferred until the current pair finishes.
    bool emittingStateSignals_ = false;
    std::atomic_bool reconnectAvailable_{false};
    std::atomic_int messageLengthMax_{0};
    std::atomic_uint64_t sendGeneration_{0};
    mutable std::mutex reconnectMutex_;
    ReconnectDelegate reconnectDelegate_;
    mutable std::mutex operationMutex_;
    std::array<OperationSlot, 2> operations_;
    std::uint64_t nextGeneration_ = 0;
    std::weak_ptr<rumble::SessionController> session_;
    std::optional<rumble::ModerationState> moderationState_;
    std::uint64_t moderationRevision_ = 0;
    // close() can be requested off-owner. This gate becomes visible before the
    // queued close task so earlier queued publication/reconnect work is inert.
    std::atomic_bool closing_{false};
    friend class RumbleChannelProvider;
};
}  // namespace chatterino
