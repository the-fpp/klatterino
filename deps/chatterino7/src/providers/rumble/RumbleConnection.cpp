// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleConnection.hpp"

#include "providers/rumble/RumbleApi.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleChannelKey.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/rumble/RumbleScheduler.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace chatterino::rumble {
namespace {

constexpr std::int64_t STREAM_END_STABLE_RESET_MS = 10'000;

RumbleFailure failureFor(const Error &error, bool exhausted = false)
{
    if (exhausted)
    {
        return {RumbleFailureCategory::Transport,
                RumbleFailureCode::RetryExhausted,
                RumbleOperatorText::RetryLimitReached, error.code};
    }
    const bool streamEnded = error.code == QStringLiteral("stream_eof") ||
                             error.code == QStringLiteral("stream_ended");
    switch (error.outcome)
    {
        case Outcome::Timeout:
            return {RumbleFailureCategory::Transport,
                    RumbleFailureCode::Timeout,
                    RumbleOperatorText::RequestTimedOut, error.code};
        case Outcome::TransportError:
            return {RumbleFailureCategory::Transport,
                    streamEnded ? RumbleFailureCode::StreamEnded
                                : RumbleFailureCode::Unavailable,
                    streamEnded
                        ? RumbleOperatorText::ConnectionEnded
                        : RumbleOperatorText::ServiceTemporarilyUnavailable,
                    error.code};
        case Outcome::HttpError:
        case Outcome::RateLimited:
            if (streamEnded)
            {
                return {RumbleFailureCategory::Transport,
                        RumbleFailureCode::StreamEnded,
                        RumbleOperatorText::ConnectionEnded, error.code};
            }
            return {RumbleFailureCategory::Transport,
                    error.retry.retryable ? RumbleFailureCode::RetryableHttp
                                          : RumbleFailureCode::RejectedHttp,
                    error.retry.retryable
                        ? RumbleOperatorText::ServiceTemporarilyUnavailable
                        : RumbleOperatorText::ServiceRejectedRequest,
                    error.code};
        case Outcome::AccessInterstitial:
            return {RumbleFailureCategory::Transport,
                    RumbleFailureCode::RejectedHttp,
                    RumbleOperatorText::ServiceRejectedRequest, error.code};
        case Outcome::MalformedSchema:
        case Outcome::InvalidMediaType:
        case Outcome::RedirectRejected:
            return {RumbleFailureCategory::Protocol,
                    RumbleFailureCode::MalformedResponse,
                    RumbleOperatorText::ResponseContractChanged, error.code};
        case Outcome::LimitExceeded:
            return {RumbleFailureCategory::Protocol,
                    RumbleFailureCode::MalformedResponse,
                    RumbleOperatorText::ResponseLimitExceeded, error.code};
        case Outcome::Cancelled:
            return {RumbleFailureCategory::Internal,
                    RumbleFailureCode::Cancelled,
                    RumbleOperatorText::OperationCancelled, error.code};
        default:
            return {RumbleFailureCategory::Resolution,
                    RumbleFailureCode::Unavailable,
                    RumbleOperatorText::ResolutionUnavailable, error.code};
    }
}

Error fallbackError(Outcome outcome, QString code, bool retryable = false)
{
    return {
        .outcome = outcome,
        .retry = {.retryable = retryable},
        .code = std::move(code),
    };
}

RumbleRetryCause retryCauseFor(const Error &error)
{
    if (error.outcome == Outcome::Cancelled)
        return RumbleRetryCause::Cancelled;
    if (error.outcome == Outcome::RateLimited)
        return RumbleRetryCause::RateLimited;
    if (error.outcome == Outcome::Timeout)
        return RumbleRetryCause::Timeout;
    if (error.code == QStringLiteral("stream_eof") ||
        error.code == QStringLiteral("stream_ended"))
        return RumbleRetryCause::StreamEnded;
    if (error.code == QStringLiteral("stream_init_missing"))
        return RumbleRetryCause::MissingInit;
    if (error.code == QStringLiteral("stream_handoff_limit"))
        return RumbleRetryCause::HandoffLimit;
    if (error.outcome == Outcome::HttpError ||
        error.outcome == Outcome::AccessInterstitial)
        return RumbleRetryCause::HttpFailure;
    if (error.outcome == Outcome::MalformedSchema ||
        error.outcome == Outcome::LimitExceeded ||
        error.outcome == Outcome::InvalidMediaType ||
        error.outcome == Outcome::RedirectRejected)
        return RumbleRetryCause::ProtocolFailure;
    if (error.outcome == Outcome::TransportError)
        return RumbleRetryCause::TransportFailure;
    return RumbleRetryCause::ResolutionFailure;
}

}  // namespace

struct RumbleConnection::State
    : std::enable_shared_from_this<RumbleConnection::State> {
    enum class RetryTarget {
        Resolve,
        Stream,
    };

    State(std::shared_ptr<RumbleChannel> channel, RumbleApi &api,
          RumbleScheduler &scheduler,
          std::shared_ptr<RumbleDispatcher> dispatcher, QString locator,
          Options options)
        : channel(std::move(channel))
        , api(api)
        , scheduler(scheduler)
        , dispatcher(std::move(dispatcher))
        , locator(std::move(locator))
        , options(options)
        , eventState(options.deduplicationCapacity)
    {
        this->options.offlineRecheckMs =
            std::max<std::int64_t>(30 * 1000, this->options.offlineRecheckMs);
        this->options.maximumConsecutiveFailures = std::max<std::uint32_t>(
            1, this->options.maximumConsecutiveFailures);
        this->options.maximumBackoffMs = std::clamp<std::int64_t>(
            this->options.maximumBackoffMs, 1000, 30 * 1000);
    }

    struct ChannelHandle final : RumbleChannelOperation {
        explicit ChannelHandle(std::shared_ptr<State> state)
            : state(std::move(state))
        {
        }

        ~ChannelHandle() override
        {
            this->cancel();
        }

        void cancel() noexcept override
        {
            auto retained = std::exchange(this->state, {});
            if (!retained)
            {
                return;
            }
            retained->requestStop(false);
        }

        void release() noexcept override
        {
            this->state.reset();
        }

        std::shared_ptr<State> state;
    };

    bool current(std::uint64_t lifecycle, std::uint64_t request) const noexcept
    {
        return this->callbackGate.load(std::memory_order_acquire) &&
               !this->stopRequested.load(std::memory_order_acquire) &&
               !this->stopped && this->lifecycleGeneration == lifecycle &&
               this->requestGeneration == request &&
               this->activeEpoch ==
                   this->requestedEpoch.load(std::memory_order_acquire);
    }

    bool currentTimer(std::uint64_t lifecycle, std::uint64_t request,
                      std::uint64_t timer) const noexcept
    {
        return this->current(lifecycle, request) &&
               this->timerGeneration == timer;
    }

    std::shared_ptr<RumbleChannel> lockChannel() const
    {
        return this->channel.lock();
    }

    bool publishLifecycle(std::uint64_t lifecycle, std::uint64_t request,
                          RumbleChannelState state,
                          RumbleLifecycleMetadata metadata,
                          std::optional<RumbleFailure> failure = std::nullopt)
    {
        if (!this->current(lifecycle, request))
            return false;
        auto target = this->lockChannel();
        if (!target || !this->token)
            return false;
        const auto expectedMetadata = metadata;
        target->publishLifecycle(*this->token, state, std::move(metadata),
                                 std::move(failure));
        if (!this->current(lifecycle, request))
            return false;
        // Publications are synchronous on the owner thread. Treat a rejected
        // state-machine edge as a failed continuation instead of starting a
        // request under a stale visible state.
        return target->lifecycleSnapshot() ==
               RumbleLifecycleSnapshot{state, expectedMetadata};
    }

    bool startOwner()
    {
        if (this->stopRequested.load(std::memory_order_acquire) ||
            this->started || this->stopped || !this->dispatcher ||
            !this->dispatcher->isOwnerThread())
        {
            return false;
        }
        auto target = this->lockChannel();
        if (!target)
            return false;

        auto begun = target->beginOperation(RumbleOperationKind::Connection);
        if (!begun)
            return false;
        this->token = *begun;
        auto attached = target->attachOperation(
            *this->token,
            std::make_unique<ChannelHandle>(this->shared_from_this()));
        if (!attached)
        {
            this->token.reset();
            return false;
        }

        this->started = true;
        this->callbackGate.store(true, std::memory_order_release);
        // requestStop() may race this owner-thread startup after the initial
        // guard. Do not publish reconnect availability once cancellation has
        // been latched; the queued stop owns operation cleanup.
        if (this->stopRequested.load(std::memory_order_acquire))
        {
            this->callbackGate.store(false, std::memory_order_release);
            return false;
        }
        this->activeEpoch =
            this->requestedEpoch.load(std::memory_order_acquire);
        const auto generationBeforeDelegate = this->lifecycleGeneration;
        const std::weak_ptr weak = this->shared_from_this();
        auto delegate = target->setReconnectDelegate([weak] {
            if (auto state = weak.lock())
                state->requestRetry({});
        });
        // Installing the delegate publishes availability synchronously. Its
        // observer may call reconnect(), invalidating this continuation.
        if (!delegate)
        {
            this->callbackGate.store(false, std::memory_order_release);
            this->stopped = true;
            std::ignore = target->completeOperation(*this->token);
            this->token.reset();
            return false;
        }
        if (this->stopped ||
            this->lifecycleGeneration != generationBeforeDelegate ||
            this->activeEpoch !=
                this->requestedEpoch.load(std::memory_order_acquire))
        {
            return !this->stopped;
        }

        const auto lifecycle = ++this->lifecycleGeneration;
        this->consecutiveFailures = 0;
        this->consecutiveStreamEndRevalidations = 0;
        this->connectedAtMs.reset();
        this->streamId.reset();
        this->beginResolve(lifecycle);
        return true;
    }

    void invalidateOwner()
    {
        ++this->lifecycleGeneration;
        ++this->requestGeneration;
        ++this->timerGeneration;
        this->apiOperation.cancel();
        if (this->timer)
        {
            this->timer->cancel();
            this->timer.reset();
        }
    }

    void requestRetry(QString replacement)
    {
        if (this->stopRequested.load(std::memory_order_acquire) ||
            !this->dispatcher)
            return;
        const auto epoch =
            this->requestedEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        const std::weak_ptr weak = this->shared_from_this();
        auto task = [weak, epoch,
                     replacement = std::move(replacement)]() mutable {
            if (auto state = weak.lock())
                state->retryOwner(epoch, std::move(replacement));
        };
        if (this->dispatcher->isOwnerThread())
            task();
        else
        {
            const bool accepted = this->dispatcher->dispatch(task);
            if (!accepted)
            {
                this->requestStop(true);
            }
        }
    }

    bool normalizeExplicitRetry(std::uint64_t lifecycle, std::uint64_t request)
    {
        if (!this->current(lifecycle, request))
            return false;
        const auto target = this->lockChannel();
        if (!target)
            return false;

        auto visibleState = target->state();
        RumbleLifecycleMetadata metadata{
            .consecutiveFailures = 0,
        };
        if (visibleState == RumbleChannelState::Connected ||
            visibleState == RumbleChannelState::Connecting)
        {
            auto interrupted = metadata;
            interrupted.retryCause = RumbleRetryCause::Cancelled;
            if (!this->publishLifecycle(lifecycle, request,
                                        RumbleChannelState::Backoff,
                                        std::move(interrupted)))
            {
                return false;
            }
            visibleState = RumbleChannelState::Backoff;
        }
        if (visibleState == RumbleChannelState::Unresolved)
            return true;
        if (visibleState != RumbleChannelState::Offline &&
            visibleState != RumbleChannelState::Backoff &&
            visibleState != RumbleChannelState::Failed)
        {
            return false;
        }
        return this->publishLifecycle(lifecycle, request,
                                      RumbleChannelState::Unresolved, metadata);
    }

    void deferRetryScheduleFailure(std::uint64_t lifecycle,
                                   std::uint64_t request)
    {
        const std::weak_ptr weak = this->shared_from_this();
        bool accepted = false;
        try
        {
            accepted = this->dispatcher->dispatch([weak, lifecycle, request] {
                const auto state = weak.lock();
                if (!state ||
                    !state->normalizeExplicitRetry(lifecycle, request))
                {
                    return;
                }
                state->failInternal(lifecycle, request,
                                    RumbleRetryCause::SchedulerUnavailable);
            });
        }
        catch (...)
        {
            accepted = false;
        }
        if (!accepted)
            this->requestStop(true);
    }

    void retryOwner(std::uint64_t epoch, QString replacement)
    {
        if (this->stopRequested.load(std::memory_order_acquire) ||
            !this->callbackGate.load(std::memory_order_acquire) ||
            this->stopped || !this->started ||
            !this->dispatcher->isOwnerThread() ||
            this->requestedEpoch.load(std::memory_order_acquire) != epoch)
        {
            return;
        }
        this->activeEpoch = epoch;
        if (!replacement.isNull())
        {
            this->locator = std::move(replacement);
            this->eventState = EventState(this->options.deduplicationCapacity);
        }
        // Explicit retry/context restart always revalidates the public
        // locator. Direct stream reconnect is reserved for automatic loss.
        this->streamId.reset();
        this->invalidateOwner();
        this->consecutiveFailures = 0;
        this->consecutiveStreamEndRevalidations = 0;
        this->connectedAtMs.reset();
        const auto lifecycle = this->lifecycleGeneration;
        const auto request = this->requestGeneration;
        const auto timerGeneration = ++this->timerGeneration;
        const std::weak_ptr weak = this->shared_from_this();
        std::unique_ptr<ScheduledTask> scheduled;
        try
        {
            scheduled = this->scheduler.scheduleAfter(
                0, [weak, lifecycle, request, timerGeneration] {
                    const auto state = weak.lock();
                    if (!state || !state->currentTimer(lifecycle, request,
                                                       timerGeneration))
                        return;
                    state->timer.reset();
                    if (!state->normalizeExplicitRetry(lifecycle, request))
                        return;
                    state->beginResolve(lifecycle);
                });
        }
        catch (...)
        {
            this->deferRetryScheduleFailure(lifecycle, request);
            return;
        }
        if (!this->currentTimer(lifecycle, request, timerGeneration))
        {
            if (scheduled)
                scheduled->cancel();
            return;
        }
        if (!scheduled)
        {
            this->deferRetryScheduleFailure(lifecycle, request);
            return;
        }
        this->timer = std::move(scheduled);
    }

    void beginResolve(std::uint64_t lifecycle)
    {
        if (this->stopRequested.load(std::memory_order_acquire) ||
            !this->callbackGate.load(std::memory_order_acquire) ||
            this->stopped || this->lifecycleGeneration != lifecycle)
            return;
        const auto request = ++this->requestGeneration;
        RumbleLifecycleMetadata metadata{
            .consecutiveFailures = this->consecutiveFailures,
        };
        const auto target = this->lockChannel();
        if (!target)
            return;
        const auto visibleState = target->lifecycleSnapshot().state;
        if (visibleState == RumbleChannelState::Connected ||
            visibleState == RumbleChannelState::Connecting)
        {
            // The public state table has no direct connected/connecting to
            // resolving edge. Explicit retry/context restart first retires
            // the read through the allowed backoff edge, then re-resolves.
            auto interrupted = metadata;
            interrupted.retryCause = RumbleRetryCause::Cancelled;
            if (!this->publishLifecycle(lifecycle, request,
                                        RumbleChannelState::Backoff,
                                        std::move(interrupted)))
            {
                return;
            }
        }
        if (!this->publishLifecycle(lifecycle, request,
                                    RumbleChannelState::Unresolved, metadata))
            return;

        const std::weak_ptr weak = this->shared_from_this();
        auto operation = this->api.resolve(
            this->locator, [weak, lifecycle, request](ResolveResult result) {
                const auto state = weak.lock();
                if (state && state->current(lifecycle, request))
                    state->resolved(lifecycle, request, std::move(result));
            });
        if (!this->current(lifecycle, request))
        {
            operation.cancel();
            return;
        }
        this->apiOperation = std::move(operation);
    }

    void resolved(std::uint64_t lifecycle, std::uint64_t request,
                  ResolveResult result)
    {
        if (!this->current(lifecycle, request))
            return;
        this->apiOperation = {};
        if (result.outcome == Outcome::ValidOffline)
        {
            this->consecutiveFailures = 0;
            this->consecutiveStreamEndRevalidations = 0;
            this->connectedAtMs.reset();
            this->streamId.reset();
            this->enterOffline(lifecycle, request);
            return;
        }
        if (result.outcome != Outcome::ResolvedLive || !result.metadata ||
            result.metadata->streamId.isEmpty())
        {
            const auto error = result.error.value_or(fallbackError(
                result.outcome, QStringLiteral("resolution_failed"), false));
            this->handleFailure(lifecycle, request, error,
                                RetryTarget::Resolve);
            return;
        }

        const auto streamKey = RumbleChannelKey::normalize(
            RumbleChannelKeyKind::StreamId, result.metadata->streamId);
        std::optional<RumbleChannelKey> channelKey;
        // `channel_id` in embed JSON is a numeric provider identity, not a
        // public channel slug. Persist a slug only from the canonical locator.
        if (result.locator.kind == LocatorKind::Channel)
        {
            auto normalized = RumbleChannelKey::normalize(
                RumbleChannelKeyKind::ChannelSlug, result.locator.value);
            if (normalized)
                channelKey = std::move(*normalized);
        }
        std::optional<RumbleChannelKey> embedKey;
        if (!result.metadata->embedId.isEmpty())
        {
            auto normalized = RumbleChannelKey::normalize(
                RumbleChannelKeyKind::EmbedId, result.metadata->embedId);
            if (normalized)
                embedKey = std::move(*normalized);
        }
        if (!streamKey)
        {
            this->failInternal(lifecycle, request,
                               RumbleRetryCause::InvalidMetadata);
            return;
        }
        auto displayName = result.metadata->channelTitle;
        if (displayName.isEmpty() &&
            result.locator.kind == LocatorKind::Channel)
        {
            displayName = result.locator.value;
        }
        if (displayName.isEmpty())
            displayName = result.metadata->videoTitle;
        if (displayName.isEmpty())
            displayName = result.locator.value;
        auto metadata = RumbleResolvedMetadata::create(
            std::move(displayName), std::move(channelKey), std::move(embedKey),
            *streamKey, result.metadata->videoTitle);
        if (!metadata)
        {
            this->failInternal(lifecycle, request,
                               RumbleRetryCause::InvalidMetadata);
            return;
        }

        this->streamId = streamKey->value();
        auto target = this->lockChannel();
        if (!target || !this->token)
            return;
        target->publishMetadata(*this->token, std::move(*metadata));
        if (!this->current(lifecycle, request))
            return;
        // The persistent locator is resolved at this point. Publish the same
        // connecting boundary used by the stream startup before fetching the
        // optional catalog so the channel picker is never gated on ancillary
        // emote metadata.
        RumbleLifecycleMetadata connecting{
            .consecutiveFailures = this->consecutiveFailures,
        };
        if (!this->publishLifecycle(lifecycle, request,
                                    RumbleChannelState::Connecting,
                                    std::move(connecting)))
            return;
        this->beginEmoteCatalog(lifecycle, *this->streamId);
    }

    void beginEmoteCatalog(std::uint64_t lifecycle, const QString &stream)
    {
        if (this->stopRequested.load(std::memory_order_acquire) ||
            !this->callbackGate.load(std::memory_order_acquire) ||
            this->stopped || this->lifecycleGeneration != lifecycle)
            return;
        // A reconnect is a catalog-generation boundary. Hide the previous
        // generation immediately so completion and newly hydrated messages
        // cannot use stale stream metadata while this bounded refresh runs.
        this->eventState.replaceEmoteCatalog(stream, {});
        auto target = this->lockChannel();
        if (!target || !this->token)
            return;
        target->publishEmoteCatalog(*this->token, {});
        const auto request = ++this->requestGeneration;
        const std::weak_ptr weak = this->shared_from_this();
        auto operation = this->api.emoteCatalog(
            stream, [weak, lifecycle, request,
                     stream](EmoteCatalogResult result) mutable {
                const auto state = weak.lock();
                if (!state || !state->current(lifecycle, request))
                    return;
                state->apiOperation = {};
                auto catalog = result.catalog.value_or(EmoteCatalog{});
                state->eventState.replaceEmoteCatalog(stream, catalog);
                auto target = state->lockChannel();
                if (!target || !state->token)
                    return;
                target->publishEmoteCatalog(*state->token, std::move(catalog));
                if (!state->current(lifecycle, request))
                    return;
                state->beginStream(lifecycle, stream);
            });
        if (!this->current(lifecycle, request))
        {
            operation.cancel();
            return;
        }
        this->apiOperation = std::move(operation);
    }

    void beginStream(std::uint64_t lifecycle, const QString &stream)
    {
        if (this->stopRequested.load(std::memory_order_acquire) ||
            !this->callbackGate.load(std::memory_order_acquire) ||
            this->stopped || this->lifecycleGeneration != lifecycle)
            return;
        const auto request = ++this->requestGeneration;
        this->seenInit = false;
        this->connectedAtMs.reset();
        RumbleLifecycleMetadata metadata{
            .consecutiveFailures = this->consecutiveFailures,
        };
        if (!this->publishLifecycle(lifecycle, request,
                                    RumbleChannelState::Connecting, metadata))
            return;

        const std::weak_ptr weak = this->shared_from_this();
        StreamCallbacks callbacks;
        callbacks.onEvents = [weak, lifecycle, request](StreamBatch batch) {
            const auto state = weak.lock();
            if (state && state->current(lifecycle, request))
                state->events(lifecycle, request, std::move(batch));
        };
        callbacks.onTerminal = [weak, lifecycle,
                                request](StreamTerminal terminal) {
            const auto state = weak.lock();
            if (state && state->current(lifecycle, request))
                state->terminal(lifecycle, request, std::move(terminal));
        };
        auto operation = this->api.stream(stream, std::move(callbacks));
        if (!this->current(lifecycle, request))
        {
            operation.cancel();
            return;
        }
        this->apiOperation = std::move(operation);
    }

    bool publishProcessed(std::uint64_t lifecycle, std::uint64_t request,
                          const Event &event, ProcessedEvent processed)
    {
        auto target = this->lockChannel();
        if (!target || !this->token)
            return false;
        const bool history = std::holds_alternative<InitEvent>(event);
        if (history)
        {
            const auto &init = std::get<InitEvent>(event);
            target->publishMessageLengthMax(*this->token,
                                            init.messageLengthMax);
            if (!this->current(lifecycle, request))
                return false;
            std::vector<RumbleMessagePublication> publications;
            publications.reserve(processed.messages.size());
            for (const auto &message : processed.messages)
            {
                auto publication = RumbleMessagePublication::fromDto(message);
                if (publication)
                    publications.push_back(std::move(*publication));
            }
            if (!publications.empty())
            {
                target->publishBootstrap(*this->token, std::move(publications));
                if (!this->current(lifecycle, request))
                    return false;
            }
        }
        else
        {
            for (const auto &message : processed.messages)
            {
                auto publication = RumbleMessagePublication::fromDto(message);
                if (!publication)
                    continue;
                target->publishRealtime(*this->token, std::move(*publication));
                if (!this->current(lifecycle, request))
                    return false;
            }
        }

        for (const auto &operation : processed.operations)
        {
            target->publishModeration(*this->token, operation);
            if (!this->current(lifecycle, request))
                return false;
            std::visit(
                [&](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, DeleteMessagesEvent>)
                    {
                        for (const auto &raw : value.messageIds)
                        {
                            auto id = RumbleMessageId::fromNormalized(raw);
                            if (id)
                                target->publishDeletion(*this->token,
                                                        std::move(*id));
                            if (!this->current(lifecycle, request))
                                return;
                        }
                    }
                    else if constexpr (std::is_same_v<
                                           T, DeleteNonRantMessagesEvent>)
                    {
                        if (!value.clearNonRant)
                            return;
                        RumbleNonRantClear clear{
                            .mode = value.messageIds.isEmpty()
                                        ? RumbleNonRantClearMode::AllKnown
                                        : RumbleNonRantClearMode::ListedIds,
                        };
                        for (const auto &raw : value.messageIds)
                        {
                            auto id = RumbleMessageId::fromNormalized(raw);
                            if (id)
                                clear.ids.push_back(std::move(*id));
                        }
                        target->publishNonRantClear(*this->token,
                                                    std::move(clear));
                    }
                    else if constexpr (std::is_same_v<T, PinMessageEvent>)
                    {
                        auto publication =
                            RumbleMessagePublication::fromDto(value.message);
                        if (publication)
                        {
                            target->publishPinnedMessage(
                                *this->token,
                                RumblePinnedMessage{std::move(*publication)});
                        }
                    }
                },
                operation);
            if (!this->current(lifecycle, request))
                return false;
        }
        return true;
    }

    void events(std::uint64_t lifecycle, std::uint64_t request,
                StreamBatch batch)
    {
        if (!this->current(lifecycle, request))
            return;
        for (const auto &event : batch.events)
        {
            const bool init = std::holds_alternative<InitEvent>(event);
            if (!this->seenInit && !init)
            {
                this->apiOperation.cancel();
                this->handleFailure(
                    lifecycle, request,
                    fallbackError(Outcome::MalformedSchema,
                                  QStringLiteral("stream_init_missing")),
                    RetryTarget::Stream);
                return;
            }
            if (init && !this->seenInit)
            {
                this->seenInit = true;
                this->consecutiveFailures = 0;
                this->connectedAtMs = this->scheduler.nowMs();
                if (!this->publishLifecycle(lifecycle, request,
                                            RumbleChannelState::Connected,
                                            RumbleLifecycleMetadata{}))
                    return;
            }
            else if (!init && this->seenInit)
            {
                // A post-init provider event proves that the revalidated
                // stream advanced beyond an immediately stale endpoint.
                this->consecutiveStreamEndRevalidations = 0;
            }
            if (const auto *messages = std::get_if<MessagesEvent>(&event))
            {
                // Commit provider catalogs once, then deduplicate and publish
                // one realtime message at a time. If the first append signal
                // reenters reconnect(), later IDs were never committed and
                // can correctly reappear in reconnect history.
                MessagesEvent catalogs{
                    .users = messages->users,
                    .channels = messages->channels,
                };
                std::ignore = this->eventState.process(
                    Event{std::move(catalogs)},
                    this->streamId.value_or(QString{}));
                for (const auto &message : messages->messages)
                {
                    MessagesEvent single;
                    single.messages.push_back(message);
                    Event staged{std::move(single)};
                    auto processed = this->eventState.process(
                        staged, this->streamId.value_or(QString{}));
                    if (!this->publishProcessed(lifecycle, request, staged,
                                                std::move(processed)))
                        return;
                }
                continue;
            }
            auto processed = this->eventState.process(
                event, this->streamId.value_or(QString{}));
            if (!this->publishProcessed(lifecycle, request, event,
                                        std::move(processed)))
                return;
        }
    }

    void terminal(std::uint64_t lifecycle, std::uint64_t request,
                  StreamTerminal terminal)
    {
        if (!this->current(lifecycle, request))
            return;
        this->apiOperation = {};
        if (terminal.outcome == Outcome::ValidOffline)
        {
            this->consecutiveFailures = 0;
            this->consecutiveStreamEndRevalidations = 0;
            this->connectedAtMs.reset();
            this->enterOffline(lifecycle, request);
            return;
        }
        auto error = terminal.error.value_or(fallbackError(
            terminal.outcome, QStringLiteral("stream_ended"), true));
        // A clean SSE EOF or a gone/not-found stream endpoint can mean either
        // an ordinary interrupted connection or that the broadcast ended.
        // Neither transport result is authoritative by itself. Re-resolve the
        // persistent public locator after the normal bounded backoff: a live
        // result reconnects without flashing offline, while ValidOffline is
        // the provider-confirmed transition that clears availability.
        const bool revalidateLocator =
            error.code == QStringLiteral("stream_eof") ||
            error.httpStatus == 404 || error.httpStatus == 410;
        const auto now = this->scheduler.nowMs();
        if (this->connectedAtMs && *this->connectedAtMs >= 0 &&
            now >= *this->connectedAtMs &&
            now - *this->connectedAtMs >= STREAM_END_STABLE_RESET_MS)
        {
            this->consecutiveStreamEndRevalidations = 0;
        }
        this->connectedAtMs.reset();
        if (revalidateLocator)
        {
            if (this->consecutiveStreamEndRevalidations <
                std::numeric_limits<std::uint32_t>::max())
            {
                ++this->consecutiveStreamEndRevalidations;
            }
            // A valid init resets ordinary transport failures, but it must not
            // make a stale live page plus immediately-ended SSE loop forever.
            this->consecutiveFailures =
                this->consecutiveStreamEndRevalidations - 1;
            error.retry.retryable = true;
            error.code = QStringLiteral("stream_ended");
        }
        this->handleFailure(
            lifecycle, request, error,
            revalidateLocator ? RetryTarget::Resolve : RetryTarget::Stream);
    }

    void enterOffline(std::uint64_t lifecycle, std::uint64_t request)
    {
        const auto now = this->scheduler.nowMs();
        if (this->options.offlineRecheckMs < 0 ||
            now > std::numeric_limits<std::int64_t>::max() -
                      this->options.offlineRecheckMs)
        {
            this->failInternal(lifecycle, request,
                               RumbleRetryCause::DeadlineOverflow);
            return;
        }
        const auto deadline = now + this->options.offlineRecheckMs;
        if (!this->installRetryTimer(lifecycle, request,
                                     this->options.offlineRecheckMs,
                                     RetryTarget::Resolve))
        {
            this->failInternal(lifecycle, request,
                               RumbleRetryCause::SchedulerUnavailable);
            return;
        }
        const auto installedTimerGeneration = this->timerGeneration;
        RumbleLifecycleMetadata metadata{
            .consecutiveFailures = 0,
            .scheduledAtMs = now,
            .deadlineAtMs = deadline,
        };
        if (!this->publishLifecycle(lifecycle, request,
                                    RumbleChannelState::Offline, metadata))
        {
            if (this->currentTimer(lifecycle, request,
                                   installedTimerGeneration) &&
                this->timer)
            {
                this->timer->cancel();
                this->timer.reset();
            }
            return;
        }
    }

    void publishFailed(std::uint64_t lifecycle, std::uint64_t request,
                       RumbleLifecycleMetadata metadata, RumbleFailure failure)
    {
        if (!this->current(lifecycle, request))
            return;
        const auto target = this->lockChannel();
        if (!target)
            return;
        const auto visibleState = target->lifecycleSnapshot().state;
        if (visibleState == RumbleChannelState::Connected)
        {
            if (!this->publishLifecycle(lifecycle, request,
                                        RumbleChannelState::Backoff, metadata))
                return;
        }
        else if (visibleState == RumbleChannelState::Offline)
        {
            if (!this->publishLifecycle(lifecycle, request,
                                        RumbleChannelState::Unresolved,
                                        metadata))
                return;
        }
        std::ignore = this->publishLifecycle(
            lifecycle, request, RumbleChannelState::Failed, std::move(metadata),
            std::move(failure));
    }

    void handleFailure(std::uint64_t lifecycle, std::uint64_t request,
                       const Error &error, RetryTarget target)
    {
        if (!this->current(lifecycle, request))
            return;
        ++this->consecutiveFailures;
        if (!error.retry.retryable)
        {
            RumbleLifecycleMetadata metadata{
                .consecutiveFailures = this->consecutiveFailures,
                .retryCause = retryCauseFor(error),
            };
            this->publishFailed(lifecycle, request, std::move(metadata),
                                failureFor(error));
            return;
        }

        if (this->consecutiveFailures >=
            this->options.maximumConsecutiveFailures)
        {
            RumbleLifecycleMetadata metadata{
                .consecutiveFailures = this->consecutiveFailures,
                .rateLimited = error.outcome == Outcome::RateLimited,
                .retryCause = retryCauseFor(error),
            };
            this->publishFailed(lifecycle, request, std::move(metadata),
                                failureFor(error, true));
            return;
        }

        std::int64_t delayMs = 0;
        if (error.retry.after)
        {
            const auto seconds = error.retry.after->count();
            if (seconds > std::numeric_limits<std::int64_t>::max() / 1000)
            {
                this->failInternal(lifecycle, request,
                                   RumbleRetryCause::DeadlineOverflow);
                return;
            }
            delayMs = seconds * 1000;
        }
        else
        {
            const auto shift =
                std::min<std::uint32_t>(this->consecutiveFailures - 1, 30);
            const auto exponential = std::min<std::int64_t>(
                this->options.maximumBackoffMs, static_cast<std::int64_t>(1000)
                                                    << shift);
            try
            {
                delayMs = static_cast<std::int64_t>(this->scheduler.randomBelow(
                    static_cast<std::uint64_t>(exponential) + 1));
            }
            catch (...)
            {
                this->failInternal(lifecycle, request,
                                   RumbleRetryCause::SchedulerUnavailable);
                return;
            }
        }
        const auto now = this->scheduler.nowMs();
        if (delayMs < 0 ||
            now > std::numeric_limits<std::int64_t>::max() - delayMs)
        {
            this->failInternal(lifecycle, request,
                               RumbleRetryCause::DeadlineOverflow);
            return;
        }
        RumbleLifecycleMetadata metadata{
            .consecutiveFailures = this->consecutiveFailures,
            .scheduledAtMs = now,
            .deadlineAtMs = now + delayMs,
            .rateLimited = error.outcome == Outcome::RateLimited,
            .retryCause = retryCauseFor(error),
        };
        if (!this->publishLifecycle(lifecycle, request,
                                    RumbleChannelState::Backoff,
                                    std::move(metadata)))
            return;
        this->scheduleRetry(lifecycle, request, delayMs, target);
    }

    bool installRetryTimer(std::uint64_t lifecycle, std::uint64_t request,
                           std::int64_t delayMs, RetryTarget target)
    {
        const auto timerGeneration = ++this->timerGeneration;
        const std::weak_ptr weak = this->shared_from_this();
        std::unique_ptr<ScheduledTask> scheduled;
        try
        {
            scheduled = this->scheduler.scheduleAfter(
                delayMs, [weak, lifecycle, request, timerGeneration, target] {
                    const auto state = weak.lock();
                    if (!state || !state->currentTimer(lifecycle, request,
                                                       timerGeneration))
                        return;
                    state->timer.reset();
                    if (target == RetryTarget::Stream && state->streamId)
                        state->beginEmoteCatalog(lifecycle, *state->streamId);
                    else
                        state->beginResolve(lifecycle);
                });
        }
        catch (...)
        {
            return false;
        }
        if (!this->currentTimer(lifecycle, request, timerGeneration))
        {
            if (scheduled)
                scheduled->cancel();
            return false;
        }
        if (!scheduled)
            return false;
        this->timer = std::move(scheduled);
        return true;
    }

    void scheduleRetry(std::uint64_t lifecycle, std::uint64_t request,
                       std::int64_t delayMs, RetryTarget target)
    {
        if (!this->installRetryTimer(lifecycle, request, delayMs, target) &&
            this->current(lifecycle, request))
        {
            this->failInternal(lifecycle, request,
                               RumbleRetryCause::SchedulerUnavailable);
        }
    }

    void failInternal(std::uint64_t lifecycle, std::uint64_t request,
                      RumbleRetryCause cause)
    {
        if (!this->current(lifecycle, request))
            return;
        RumbleLifecycleMetadata metadata{
            .consecutiveFailures = this->consecutiveFailures,
            .retryCause = cause,
        };
        this->publishFailed(
            lifecycle, request, std::move(metadata),
            RumbleFailure{RumbleFailureCategory::Internal,
                          RumbleFailureCode::InvariantViolation,
                          RumbleOperatorText::InternalStateError});
    }

    void requestStop(bool closeChannel)
    {
        if (this->stopRequested.exchange(true, std::memory_order_acq_rel))
            return;
        this->callbackGate.store(false, std::memory_order_release);
        if (!this->dispatcher)
            return;
        const auto retained = this->shared_from_this();
        if (this->dispatcher->isOwnerThread())
        {
            this->stopOwner(closeChannel);
            return;
        }
        const bool accepted =
            this->dispatcher->dispatch([retained, closeChannel] {
                retained->stopOwner(closeChannel);
            });
        if (!accepted)
        {
            this->dispatcher->dispose([retained, closeChannel] {
                retained->stopOwner(closeChannel);
            });
        }
    }

    void stopOwner(bool closeChannel)
    {
        this->stopRequested.store(true, std::memory_order_release);
        this->callbackGate.store(false, std::memory_order_release);
        if (this->stopped)
            return;
        this->stopped = true;
        ++this->lifecycleGeneration;
        ++this->requestGeneration;
        ++this->timerGeneration;
        this->apiOperation.cancel();
        if (this->timer)
        {
            this->timer->cancel();
            this->timer.reset();
        }
        auto target = this->lockChannel();
        if (target)
        {
            if (closeChannel)
            {
                // Channel close owns the committed Closed/state/live/
                // reconnect signal ordering and clears the delegate itself.
                target->close();
            }
            else
            {
                // Operation replacement keeps the channel alive but must
                // retire the stale reconnect target. The gate is already
                // closed, so any availability-signal reentrancy is inert.
                std::ignore = target->setReconnectDelegate({});
            }
        }
        this->token.reset();
    }

    std::weak_ptr<RumbleChannel> channel;
    RumbleApi &api;
    RumbleScheduler &scheduler;
    std::shared_ptr<RumbleDispatcher> dispatcher;
    QString locator;
    Options options;
    EventState eventState;
    std::atomic_bool stopRequested{false};
    std::atomic_bool callbackGate{false};
    std::atomic<std::uint64_t> requestedEpoch{0};
    std::uint64_t activeEpoch = 0;
    bool started = false;
    bool stopped = false;
    bool seenInit = false;
    std::uint64_t lifecycleGeneration = 0;
    std::uint64_t requestGeneration = 0;
    std::uint64_t timerGeneration = 0;
    std::uint32_t consecutiveFailures = 0;
    std::uint32_t consecutiveStreamEndRevalidations = 0;
    std::optional<std::int64_t> connectedAtMs;
    std::optional<QString> streamId;
    std::optional<RumbleOperationToken> token;
    Cancellation apiOperation;
    std::unique_ptr<ScheduledTask> timer;
};

RumbleConnection::RumbleConnection(std::shared_ptr<RumbleChannel> channel,
                                   RumbleApi &api, RumbleScheduler &scheduler,
                                   std::shared_ptr<RumbleDispatcher> dispatcher,
                                   QString locator)
    : RumbleConnection(std::move(channel), api, scheduler,
                       std::move(dispatcher), std::move(locator), Options{})
{
}

RumbleConnection::RumbleConnection(std::shared_ptr<RumbleChannel> channel,
                                   RumbleApi &api, RumbleScheduler &scheduler,
                                   std::shared_ptr<RumbleDispatcher> dispatcher,
                                   QString locator, Options options)
    : state_(std::make_shared<State>(std::move(channel), api, scheduler,
                                     std::move(dispatcher), std::move(locator),
                                     options))
{
}

RumbleConnection::~RumbleConnection()
{
    this->stop();
}

bool RumbleConnection::start()
{
    return this->state_ && this->state_->startOwner();
}

void RumbleConnection::retry()
{
    if (this->state_)
        this->state_->requestRetry({});
}

void RumbleConnection::restart(QString locator)
{
    if (this->state_)
        this->state_->requestRetry(std::move(locator));
}

void RumbleConnection::retire()
{
    if (this->state_)
        this->state_->requestStop(false);
}

void RumbleConnection::stop()
{
    if (this->state_)
        this->state_->requestStop(true);
}

}  // namespace chatterino::rumble
