// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleApplicationController.hpp"

#include "common/WindowDescriptors.hpp"
#include "providers/rumble/RumbleAccount.hpp"
#include "providers/rumble/RumbleAccountManager.hpp"
#include "providers/rumble/RumbleApi.hpp"
#include "providers/rumble/RumbleBrowserLogin.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleConnection.hpp"
#include "providers/rumble/RumbleDiagnostics.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/rumble/RumbleQtAuthTransport.hpp"
#include "providers/rumble/RumbleQtTransport.hpp"
#include "providers/rumble/RumbleScheduler.hpp"
#include "providers/rumble/RumbleSession.hpp"
#include "util/MultiChannel.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chatterino {

namespace {

constexpr auto RUMBLE_LAYOUT_PLATFORM = "rumble";

class RumbleLayoutPlaceholder final : public Channel
{
public:
    RumbleLayoutPlaceholder()
        : Channel(QStringLiteral("rumble-unavailable"), Type::Rumble)
    {
    }

    const QString &getDisplayName() const override
    {
        static const QString text =
            QStringLiteral("Rumble channel unavailable");
        return text;
    }

    const QString &getLocalizedName() const override
    {
        return this->getDisplayName();
    }

    bool canReconnect() const override
    {
        return true;
    }
};

RumbleLayoutResolveError locatorError(RumbleLayoutLocatorError error)
{
    return {
        .code = error == RumbleLayoutLocatorError::DirectStreamNotPersistable
                    ? RumbleLayoutResolveErrorCode::DirectStreamNotPersistable
                    : RumbleLayoutResolveErrorCode::InvalidLocator,
        .userMessage = rumbleLayoutErrorText(error),
    };
}

RumbleLayoutResolveError providerUnavailable()
{
    return {
        .code = RumbleLayoutResolveErrorCode::ProviderUnavailable,
        .userMessage = QStringLiteral(
            "Rumble is shutting down. Try again after restarting Chatterino."),
    };
}

RumbleLayoutResolveError resolutionFailed(
    const std::shared_ptr<RumbleChannel> &channel)
{
    QString text = QStringLiteral(
        "Rumble couldn't find that channel or video. Check the link and try "
        "again.");
    if (channel && channel->failure())
    {
        if (auto safe = channel->failure()->operatorSafeText())
        {
            text = *safe;
        }
    }
    return {
        .code = RumbleLayoutResolveErrorCode::ResolutionFailed,
        .userMessage = std::move(text),
    };
}

IndirectChannel makePlaceholderImpl(QString canonicalLocator)
{
    return IndirectChannel(
        std::make_shared<RumbleLayoutPlaceholder>(), Channel::Type::Rumble,
        ChannelLayoutIdentity{
            .platform = QString::fromLatin1(RUMBLE_LAYOUT_PLATFORM),
            .locator = std::move(canonicalLocator),
        });
}

}  // namespace

IndirectChannel makeRumbleLayoutPlaceholder(QString canonicalLocator)
{
    const auto validated = RumbleLayoutLocator::fromPersisted(canonicalLocator);
    return makePlaceholderImpl(validated ? validated->canonicalUrl()
                                         : QString{});
}

struct RumbleApplicationController::Request::State {
    std::atomic_bool active{true};
    std::mutex callbackMutex;
    std::condition_variable callbackFinished;
    bool callbackRunning = false;
    std::thread::id callbackThread;
    QString canonicalLocator;
    ResolveCallback callback;

    ResolveCallback claimCallback()
    {
        std::lock_guard lock(this->callbackMutex);
        if (!this->active.exchange(false, std::memory_order_acq_rel))
        {
            return {};
        }
        auto claimed = std::move(this->callback);
        if (claimed)
        {
            this->callbackRunning = true;
            this->callbackThread = std::this_thread::get_id();
        }
        return claimed;
    }

    void completeCallback() noexcept
    {
        {
            std::lock_guard lock(this->callbackMutex);
            this->callbackRunning = false;
            this->callbackThread = {};
        }
        this->callbackFinished.notify_all();
    }

    void cancel() noexcept
    {
        const bool owned =
            this->active.exchange(false, std::memory_order_acq_rel);
        std::unique_lock lock(this->callbackMutex);
        if (!owned && this->callbackRunning &&
            this->callbackThread != std::this_thread::get_id())
        {
            this->callbackFinished.wait(lock, [this] {
                return !this->callbackRunning;
            });
        }
        if (owned)
        {
            this->callback = {};
        }
    }
};

struct RumbleApplicationController::State
    : std::enable_shared_from_this<RumbleApplicationController::State> {
    struct Lifecycle {
        std::shared_ptr<RumbleChannel> channel;
        std::unique_ptr<rumble::RumbleConnection> connection;
        pajlada::Signals::ScopedConnection stateConnection;
        pajlada::Signals::ScopedConnection aliasConnection;
        std::vector<std::weak_ptr<Request::State>> waiters;
        bool startFailed = false;
    };

    std::unique_ptr<QObject> privateOwner;
    QObject *owner = nullptr;
    std::unique_ptr<rumble::RumbleQtTransport> ownedTransport;
    std::unique_ptr<rumble::RumbleQtAuthTransport> ownedAuthTransport;
    std::unique_ptr<rumble::BrowserLogin> browserLogin;
    std::shared_ptr<rumble::SessionController> session;
    RumbleAccountManager *accountManager = nullptr;
    pajlada::Signals::ScopedConnection accountConnection;
    std::unique_ptr<rumble::RumbleApi> ownedApi;
    std::unique_ptr<rumble::QtRumbleScheduler> ownedScheduler;
    rumble::RumbleApi *api = nullptr;
    rumble::RumbleScheduler *scheduler = nullptr;
    std::shared_ptr<RumbleDispatcher> dispatcher;
    std::unique_ptr<RumbleChannelProvider> provider;
    ChannelFactory channelFactory;

    std::map<QString, std::shared_ptr<Lifecycle>> locatorLifecycles;
    std::unordered_map<RumbleChannel *, std::weak_ptr<Lifecycle>>
        channelLifecycles;
    std::vector<std::weak_ptr<Request::State>> requests;
    std::atomic_bool callbackGate{true};
    bool providerClosed = false;

    void trackRequest(const std::shared_ptr<Request::State> &request)
    {
        std::erase_if(this->requests, [](const auto &weak) {
            return weak.expired();
        });
        this->requests.emplace_back(request);
    }

    explicit State(QObject *affinityOwner, RumbleAccountManager *accountManager)
        : accountManager(accountManager)
    {
        if (affinityOwner == nullptr)
        {
            this->privateOwner = std::make_unique<QObject>();
            affinityOwner = this->privateOwner.get();
        }
        this->owner = affinityOwner;
        this->dispatcher = makeQtRumbleDispatcher(this->owner);
        this->ownedTransport =
            std::make_unique<rumble::RumbleQtTransport>(this->owner);
        this->ownedAuthTransport =
            std::make_unique<rumble::RumbleQtAuthTransport>(this->owner);
        this->browserLogin =
            std::make_unique<rumble::BrowserLogin>(this->owner);
        this->session = std::make_shared<rumble::SessionController>(
            *this->ownedAuthTransport);
        const auto guardedOwner = QPointer<QObject>(this->owner);
        this->ownedApi = std::make_unique<rumble::RumbleApi>(
            *this->ownedTransport,
            [guardedOwner](std::function<void()> task) mutable {
                if (guardedOwner)
                {
                    QTimer::singleShot(0, guardedOwner, std::move(task));
                }
            });
        this->ownedScheduler =
            std::make_unique<rumble::QtRumbleScheduler>(this->owner);
        this->api = this->ownedApi.get();
        this->scheduler = this->ownedScheduler.get();
        this->provider =
            std::make_unique<RumbleChannelProvider>(this->dispatcher);
    }

    State(rumble::RumbleApi &api, rumble::RumbleScheduler &scheduler,
          std::shared_ptr<RumbleDispatcher> dispatcher,
          ChannelFactory channelFactory)
        : api(&api)
        , scheduler(&scheduler)
        , dispatcher(std::move(dispatcher))
        , channelFactory(std::move(channelFactory))
    {
        this->provider =
            std::make_unique<RumbleChannelProvider>(this->dispatcher);
    }

    void activateSelectedAccount()
    {
        if (!this->session || !this->accountManager ||
            !this->callbackGate.load(std::memory_order_acquire))
        {
            return;
        }
        const auto account = this->accountManager->current();
        if (!account)
        {
            this->session->clear();
            return;
        }
        if (this->session->state() == rumble::SessionState::Valid)
        {
            const auto identity = this->session->identity();
            if (identity && identity->userID == account->userID())
            {
                return;
            }
        }
        auto credential = this->accountManager->currentCredential();
        if (!credential)
        {
            this->session->clear();
            return;
        }
        if (!this->session->importSession(std::move(*credential)))
        {
            return;
        }
        this->session->validate({});
    }

    ~State()
    {
        this->shutdown();
    }

    IndirectChannel view(const QString &canonical,
                         const std::shared_ptr<Lifecycle> &lifecycle) const
    {
        return IndirectChannel(
            lifecycle->channel, Channel::Type::Rumble,
            ChannelLayoutIdentity{
                .platform = QString::fromLatin1(RUMBLE_LAYOUT_PLATFORM),
                .locator = canonical,
            });
    }

    std::shared_ptr<RumbleChannel> createChannel(
        const RumbleLayoutLocator &locator)
    {
        if (this->channelFactory)
        {
            return this->channelFactory(locator);
        }
        auto channel = this->provider->getOrCreate(locator.channelKey());
        if (!channel)
        {
            return nullptr;
        }
        (*channel)->setSessionController(this->session);
        return std::move(*channel);
    }

    void registerAliases(const std::shared_ptr<Lifecycle> &lifecycle)
    {
        if (!lifecycle || !lifecycle->channel || this->providerClosed)
        {
            return;
        }
        const auto &metadata = lifecycle->channel->metadata();
        if (!metadata)
        {
            return;
        }
        if (metadata->channelSlug())
        {
            std::ignore = this->provider->associateAlias(
                lifecycle->channel, *metadata->channelSlug());
        }
        if (metadata->embedId())
        {
            std::ignore = this->provider->associateAlias(lifecycle->channel,
                                                         *metadata->embedId());
        }
        if (metadata->streamId())
        {
            std::ignore = this->provider->associateAlias(lifecycle->channel,
                                                         *metadata->streamId());
        }
    }

    std::shared_ptr<Lifecycle> lifecycle(const RumbleLayoutLocator &locator)
    {
        const auto &canonical = locator.canonicalUrl();
        if (const auto found = this->locatorLifecycles.find(canonical);
            found != this->locatorLifecycles.end())
        {
            if (found->second->channel->state() != RumbleChannelState::Closed)
            {
                return found->second;
            }
            this->channelLifecycles.erase(found->second->channel.get());
            this->locatorLifecycles.erase(found);
        }
        if (this->providerClosed || !this->callbackGate.load())
        {
            return nullptr;
        }

        auto channel = this->createChannel(locator);
        if (!channel)
        {
            return nullptr;
        }

        std::shared_ptr<Lifecycle> result;
        if (const auto found = this->channelLifecycles.find(channel.get());
            found != this->channelLifecycles.end())
        {
            result = found->second.lock();
        }
        if (!result)
        {
            result = std::make_shared<Lifecycle>();
            result->channel = channel;
            result->connection = std::make_unique<rumble::RumbleConnection>(
                channel, *this->api, *this->scheduler, this->dispatcher,
                canonical);

            const std::weak_ptr weak = this->shared_from_this();
            const std::weak_ptr weakLifecycle = result;
            result->stateConnection = channel->stateChanged.connect(
                [weak, weakLifecycle](RumbleChannelState,
                                      RumbleChannelState state) {
                    auto self = weak.lock();
                    auto lifecycle = weakLifecycle.lock();
                    if (self && lifecycle)
                    {
                        self->stateChanged(lifecycle, state);
                    }
                });
            result->aliasConnection =
                channel->locatorChanged.connect([weak, weakLifecycle] {
                    auto self = weak.lock();
                    auto lifecycle = weakLifecycle.lock();
                    if (self && lifecycle)
                    {
                        self->registerAliases(lifecycle);
                    }
                });
            this->channelLifecycles.insert_or_assign(channel.get(), result);
            if (!result->connection->start())
            {
                result->connection.reset();
                result->startFailed = true;
                std::ignore = channel->transitionTo(
                    RumbleChannelState::Failed,
                    RumbleFailure(RumbleFailureCategory::Internal,
                                  RumbleFailureCode::InvariantViolation,
                                  RumbleOperatorText::InternalStateError));
            }
        }
        this->locatorLifecycles.emplace(canonical, result);
        return result;
    }

    static bool pickerComplete(RumbleChannelState state)
    {
        // Adding a channel requires successful public locator resolution, not
        // a completed anonymous chat-stream handshake. Connecting means the
        // page/embed metadata is valid and the runtime now owns the SSE work.
        return state == RumbleChannelState::Connecting ||
               state == RumbleChannelState::Connected ||
               state == RumbleChannelState::Offline ||
               state == RumbleChannelState::Failed ||
               state == RumbleChannelState::Closed;
    }

    void finish(const std::shared_ptr<Request::State> &request,
                const std::shared_ptr<Lifecycle> &lifecycle,
                RumbleChannelState state)
    {
        if (!request || !this->callbackGate.load(std::memory_order_acquire))
        {
            if (request)
            {
                request->cancel();
            }
            return;
        }
        auto callback = request->claimCallback();
        if (!callback)
        {
            return;
        }
        if (state == RumbleChannelState::Connecting ||
            state == RumbleChannelState::Connected ||
            state == RumbleChannelState::Offline)
        {
            try
            {
                callback(this->view(request->canonicalLocator, lifecycle));
            }
            catch (...)
            {
                request->completeCallback();
                throw;
            }
        }
        else
        {
            try
            {
                callback(makeUnexpected(resolutionFailed(lifecycle->channel)));
            }
            catch (...)
            {
                request->completeCallback();
                throw;
            }
        }
        request->completeCallback();
    }

    void stateChanged(const std::shared_ptr<Lifecycle> &lifecycle,
                      RumbleChannelState state)
    {
        if (!pickerComplete(state))
        {
            return;
        }
        auto waiters = std::move(lifecycle->waiters);
        lifecycle->waiters.clear();
        for (const auto &weak : waiters)
        {
            if (auto request = weak.lock())
            {
                const std::weak_ptr self = this->shared_from_this();
                bool accepted = false;
                try
                {
                    accepted = this->dispatcher->dispatch(
                        [self, request, lifecycle, state] {
                            if (auto retained = self.lock())
                            {
                                retained->finish(request, lifecycle, state);
                            }
                            else
                            {
                                request->cancel();
                            }
                        });
                }
                catch (...)
                {
                    accepted = false;
                }
                if (!accepted)
                {
                    request->cancel();
                }
            }
        }
    }

    void enqueueFailure(const std::shared_ptr<Request::State> &request,
                        RumbleLayoutResolveError error)
    {
        const std::weak_ptr weak = this->shared_from_this();
        const bool accepted = this->dispatcher->dispatch(
            [weak, request, error = std::move(error)]() mutable {
                auto self = weak.lock();
                if (!self ||
                    !self->callbackGate.load(std::memory_order_acquire))
                {
                    request->cancel();
                    return;
                }
                auto callback = request->claimCallback();
                if (!callback)
                {
                    return;
                }
                try
                {
                    callback(makeUnexpected(std::move(error)));
                }
                catch (...)
                {
                    request->completeCallback();
                    throw;
                }
                request->completeCallback();
            });
        if (!accepted)
        {
            request->cancel();
        }
    }

    void beginShutdown() noexcept
    {
        if (!this->callbackGate.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }
        if (this->session)
        {
            this->session->shutdown();
        }
        if (this->browserLogin)
        {
            this->browserLogin->shutdown();
        }
        for (const auto &weak : this->requests)
        {
            if (auto request = weak.lock())
            {
                request->cancel();
            }
        }
        this->requests.clear();
        for (auto &[locator, lifecycle] : this->locatorLifecycles)
        {
            (void)locator;
            for (const auto &weak : lifecycle->waiters)
            {
                if (auto request = weak.lock())
                {
                    request->cancel();
                }
            }
            lifecycle->waiters.clear();
            if (lifecycle->connection)
            {
                lifecycle->connection->retire();
            }
        }
    }

    void shutdown() noexcept
    {
        this->beginShutdown();
        if (std::exchange(this->providerClosed, true))
        {
            return;
        }
        for (const auto &[channel, weak] : this->channelLifecycles)
        {
            (void)channel;
            if (const auto lifecycle = weak.lock();
                lifecycle && lifecycle->channel)
            {
                lifecycle->channel->close();
            }
        }
        if (this->provider)
        {
            this->provider->shutdown();
        }
    }
};

RumbleApplicationController::Request::Request(std::shared_ptr<State> state)
    : state_(std::move(state))
{
}

RumbleApplicationController::Request::Request(Request &&other) noexcept
    : state_(std::exchange(other.state_, {}))
{
}

RumbleApplicationController::Request &
    RumbleApplicationController::Request::operator=(Request &&other) noexcept
{
    if (this != &other)
    {
        this->cancel();
        this->state_ = std::exchange(other.state_, {});
    }
    return *this;
}

RumbleApplicationController::Request::~Request()
{
    this->cancel();
}

void RumbleApplicationController::Request::cancel() noexcept
{
    if (this->state_)
    {
        this->state_->cancel();
        this->state_.reset();
    }
}

bool RumbleApplicationController::Request::active() const noexcept
{
    return this->state_ && this->state_->active.load(std::memory_order_acquire);
}

RumbleApplicationController::RumbleApplicationController(
    QObject *owner, RumbleAccountManager *accountManager)
    : state_(std::make_shared<State>(owner, accountManager))
{
    const std::weak_ptr weak = this->state_;
    this->state_->session->setChanged([weak] {
        if (const auto state = weak.lock())
        {
            std::vector<std::shared_ptr<RumbleChannel>> channels;
            for (const auto &[channel, lifecycleWeak] :
                 state->channelLifecycles)
            {
                (void)channel;
                if (const auto lifecycle = lifecycleWeak.lock();
                    lifecycle && lifecycle->channel)
                {
                    channels.push_back(lifecycle->channel);
                }
            }
            const auto nowUtc = QDateTime::currentDateTimeUtc();
            for (const auto &channel : channels)
            {
                channel->refreshEmoteEligibility();
                channel->writabilityChanged.invoke();
                if (const auto status = rumble::captureStatus(*channel, nowUtc))
                    rumble::logStatus(*status, nowUtc.toMSecsSinceEpoch());
            }
        }
    });
    if (accountManager)
    {
        this->state_->accountConnection =
            accountManager->currentUserChanged.connect([weak] {
                if (const auto state = weak.lock())
                {
                    state->activateSelectedAccount();
                }
            });
    }
}

RumbleApplicationController::RumbleApplicationController(
    rumble::RumbleApi &api, rumble::RumbleScheduler &scheduler,
    std::shared_ptr<RumbleDispatcher> dispatcher, ChannelFactory channelFactory)
    : state_(std::make_shared<State>(api, scheduler, std::move(dispatcher),
                                     std::move(channelFactory)))
{
}

RumbleApplicationController::~RumbleApplicationController()
{
    this->shutdown();
}

IndirectChannel RumbleApplicationController::restore(
    const QString &savedLocator)
{
    auto locator = RumbleLayoutLocator::fromPersisted(savedLocator);
    if (!locator)
    {
        return makeRumbleLayoutPlaceholder();
    }
    if (!this->state_ ||
        !this->state_->callbackGate.load(std::memory_order_acquire))
    {
        return makeRumbleLayoutPlaceholder(locator->canonicalUrl());
    }
    auto lifecycle = this->state_->lifecycle(*locator);
    if (!lifecycle)
    {
        return makeRumbleLayoutPlaceholder(locator->canonicalUrl());
    }
    return this->state_->view(locator->canonicalUrl(), lifecycle);
}

RumbleApplicationController::Request RumbleApplicationController::resolve(
    const QString &userInput, ResolveCallback callback)
{
    auto request = std::make_shared<Request::State>();
    request->callback = std::move(callback);

    if (this->state_)
    {
        this->state_->trackRequest(request);
    }

    if (!this->state_ ||
        !this->state_->callbackGate.load(std::memory_order_acquire))
    {
        if (this->state_)
        {
            this->state_->enqueueFailure(request, providerUnavailable());
        }
        else
        {
            request->active.store(false);
        }
        return Request(std::move(request));
    }

    auto locator = RumbleLayoutLocator::fromUserInput(userInput);
    if (!locator)
    {
        this->state_->enqueueFailure(request, locatorError(locator.error()));
        return Request(std::move(request));
    }
    request->canonicalLocator = locator->canonicalUrl();

    auto lifecycle = this->state_->lifecycle(*locator);
    if (!lifecycle)
    {
        this->state_->enqueueFailure(request, providerUnavailable());
        return Request(std::move(request));
    }
    lifecycle->waiters.emplace_back(request);
    const auto current = lifecycle->channel->state();
    if (current == RumbleChannelState::Failed && lifecycle->connection)
    {
        // A fresh picker acceptance is an explicit new generation. Do not
        // complete from the cached terminal snapshot.
        lifecycle->connection->restart(locator->canonicalUrl());
    }
    else if (State::pickerComplete(current) || lifecycle->startFailed)
    {
        const std::weak_ptr weak = this->state_;
        const bool accepted = this->state_->dispatcher->dispatch(
            [weak, request, lifecycle, current] {
                if (auto state = weak.lock())
                {
                    state->finish(request, lifecycle, current);
                }
            });
        if (!accepted)
        {
            request->cancel();
        }
    }
    return Request(std::move(request));
}

void RumbleApplicationController::retry(const IndirectChannel &channel)
{
    if (const auto &identity = channel.layoutIdentity();
        identity &&
        identity->platform == QString::fromLatin1(RUMBLE_LAYOUT_PLATFORM))
    {
        this->retry(identity->locator);
        return;
    }
    channel.get()->reconnect();
}

void RumbleApplicationController::retry(const QString &canonicalLocator)
{
    if (!this->state_ ||
        !this->state_->callbackGate.load(std::memory_order_acquire))
    {
        return;
    }
    auto locator = RumbleLayoutLocator::fromPersisted(canonicalLocator);
    if (!locator)
    {
        return;
    }
    auto lifecycle = this->state_->lifecycle(*locator);
    if (lifecycle && lifecycle->connection)
    {
        lifecycle->connection->restart(locator->canonicalUrl());
    }
}

void RumbleApplicationController::beginShutdown() noexcept
{
    if (this->state_)
    {
        this->state_->beginShutdown();
    }
}

void RumbleApplicationController::shutdown() noexcept
{
    if (this->state_)
    {
        this->state_->shutdown();
    }
}

bool RumbleApplicationController::isShuttingDown() const noexcept
{
    return !this->state_ ||
           !this->state_->callbackGate.load(std::memory_order_acquire);
}

bool RumbleApplicationController::importSession(QByteArray bearer)
{
    return this->state_ && this->state_->session &&
           this->state_->session->importSession(std::move(bearer));
}

void RumbleApplicationController::prepareBrowserLogin(
    std::function<void(rumble::BrowserLoginStatus)> callback)
{
    if (!this->state_ || !this->state_->browserLogin)
    {
        callback({
            .readiness = rumble::BrowserLoginReadiness::Unavailable,
            .userMessage = QStringLiteral("Rumble sign-in is unavailable."),
        });
        return;
    }
    this->state_->browserLogin->prepare(std::move(callback));
}

rumble::BrowserLoginStatus RumbleApplicationController::browserLoginStatus()
    const
{
    return this->state_ && this->state_->browserLogin
               ? this->state_->browserLogin->status()
               : rumble::BrowserLoginStatus{
                     .readiness = rumble::BrowserLoginReadiness::Unavailable,
                     .userMessage =
                         QStringLiteral("Rumble sign-in is unavailable."),
                 };
}

void RumbleApplicationController::loginInBrowser(
    std::function<void(bool, QString)> callback)
{
    const auto state = this->state_;
    if (!state || !state->session || !state->browserLogin ||
        !state->callbackGate.load(std::memory_order_acquire))
    {
        if (callback)
        {
            callback(false, QStringLiteral("Rumble sign-in is unavailable."));
        }
        return;
    }
    const std::weak_ptr weak = state;
    state->browserLogin->start([weak, callback = std::move(callback)](
                                   rumble::BrowserLoginResult result) mutable {
        const auto self = weak.lock();
        if (!self || !self->callbackGate.load(std::memory_order_acquire))
        {
            result.session.fill('\0');
            result.session.clear();
            return;
        }
        if (result.outcome != rumble::BrowserLoginOutcome::Session)
        {
            if (callback)
            {
                callback(false, std::move(result.userMessage));
            }
            return;
        }
        auto credential = result.session;
        auto browserAccountName = std::move(result.accountName);
        if (!self->session->importSession(std::move(result.session)))
        {
            credential.fill('\0');
            credential.clear();
            if (callback)
            {
                callback(
                    false,
                    QStringLiteral(
                        "Rumble sign-in could not be completed. Try again."));
            }
            return;
        }
        self->session->validate([weak, credential = std::move(credential),
                                 browserAccountName =
                                     std::move(browserAccountName),
                                 callback = std::move(callback)](
                                    bool ok, QString message) mutable {
            const auto self = weak.lock();
            if (!self || !self->callbackGate.load(std::memory_order_acquire))
            {
                credential.fill('\0');
                credential.clear();
                return;
            }
            if (!ok)
            {
                credential.fill('\0');
                credential.clear();
                self->session->clear();
                self->activateSelectedAccount();
                if (callback)
                {
                    callback(false, std::move(message));
                }
                return;
            }
            const auto identity = self->session->identity();
            if (!self->accountManager)
            {
                credential.fill('\0');
                credential.clear();
                if (callback)
                {
                    callback(true, std::move(message));
                }
                return;
            }
            if (!identity ||
                (identity->username.isEmpty() &&
                 browserAccountName.isEmpty()))
            {
                credential.fill('\0');
                credential.clear();
                self->session->clear();
                self->activateSelectedAccount();
                if (callback)
                {
                    callback(false, QStringLiteral(
                                        "Rumble sign-in couldn't confirm your "
                                        "account. Try again."));
                }
                return;
            }
            auto accountName = identity->username.isEmpty()
                                   ? std::move(browserAccountName)
                                   : identity->username;
            self->accountManager->addValidatedAccount(
                identity->userID, std::move(accountName),
                std::move(credential),
                [weak, callback = std::move(callback)](
                    RumbleAccountAddResult result) mutable {
                    if (!result.ok)
                    {
                        if (const auto self = weak.lock();
                            self &&
                            self->callbackGate.load(std::memory_order_acquire))
                        {
                            self->session->clear();
                            self->activateSelectedAccount();
                        }
                    }
                    if (callback)
                    {
                        callback(result.ok, std::move(result.userMessage));
                    }
                });
        });
    });
}

void RumbleApplicationController::cancelBrowserLogin() noexcept
{
    if (this->state_ && this->state_->browserLogin)
    {
        this->state_->browserLogin->cancel();
    }
}

bool RumbleApplicationController::browserLoginActive() const noexcept
{
    return this->state_ && this->state_->browserLogin &&
           this->state_->browserLogin->active();
}

void RumbleApplicationController::validateSession(
    std::function<void(bool, QString)> callback)
{
    if (!this->state_ || !this->state_->session)
    {
        if (callback)
            callback(false,
                     QStringLiteral("Rumble sign-in is unavailable. Try again."));
        return;
    }
    this->state_->session->validate(std::move(callback));
}

void RumbleApplicationController::clearSession() noexcept
{
    if (this->state_ && this->state_->browserLogin)
        this->state_->browserLogin->cancel();
    if (this->state_ && this->state_->accountManager)
        this->state_->accountManager->selectAccount({});
    else if (this->state_ && this->state_->session)
        this->state_->session->clear();
}

rumble::SessionState RumbleApplicationController::sessionState() const noexcept
{
    return this->state_ && this->state_->session
               ? this->state_->session->state()
               : rumble::SessionState::Empty;
}

bool rumbleLayoutNeedsPickerRepair(
    const IndirectChannel &channel,
    const RumbleApplicationController *controller)
{
    const auto validIdentity = [](const auto &identity) {
        return identity && identity->platform == QStringLiteral("rumble") &&
               RumbleLayoutLocator::fromPersisted(identity->locator);
    };

    if (channel.getType() == Channel::Type::Rumble)
    {
        return controller == nullptr ||
               !validIdentity(channel.layoutIdentity());
    }
    if (channel.getType() != Channel::Type::Multi)
    {
        return false;
    }

    const auto *multi = dynamic_cast<const MultiChannel *>(channel.get().get());
    if (!multi)
    {
        return true;
    }
    return std::ranges::any_of(
        multi->channels(), [controller, &validIdentity](const auto &child) {
            if (child.platform != MultiChannel::Platform::Rumble)
            {
                return false;
            }
            const auto descriptor = child.spec().descriptor();
            return controller == nullptr ||
                   !validIdentity(descriptor.layoutIdentity);
        });
}

}  // namespace chatterino
