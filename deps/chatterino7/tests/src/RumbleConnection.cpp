// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleConnection.hpp"

#include "lib/RumbleFixtureApiTransport.hpp"
#include "lib/RumbleFixtureLoader.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Logging.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/rumble/RumbleScheduler.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

using namespace chatterino;
using namespace chatterino::rumble;
using namespace chatterino::test;

namespace {

class MockApplication final : public mock::BaseApplication
{
public:
    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    mock::EmptyLogging logging;
};

class OwnerDispatcher final : public RumbleDispatcher
{
public:
    bool isOwnerThread() const noexcept override
    {
        return this->owner;
    }

    bool dispatch(Task task) override
    {
        if (!this->accept)
            return false;
        this->tasks.push_back(std::move(task));
        return true;
    }

    void dispose(Task cleanup) noexcept override
    {
        this->tasks.push_back(std::move(cleanup));
    }

    void runAll()
    {
        while (!this->tasks.empty())
        {
            auto task = std::move(this->tasks.front());
            this->tasks.erase(this->tasks.begin());
            task();
        }
    }

    bool owner = true;
    bool accept = true;
    std::vector<Task> tasks;
};

struct FixtureScheduledState {
    ManualScheduler *scheduler = nullptr;
    ManualScheduler::TaskId id = 0;
    std::atomic_bool active{true};
    std::function<void()> callback;
};

class FixtureScheduledTask final : public ScheduledTask
{
public:
    explicit FixtureScheduledTask(std::shared_ptr<FixtureScheduledState> state)
        : state_(std::move(state))
    {
    }

    ~FixtureScheduledTask() override
    {
        this->cancel();
    }

    void cancel() noexcept override
    {
        if (this->state_ &&
            this->state_->active.exchange(false, std::memory_order_acq_rel))
            this->state_->scheduler->cancel(this->state_->id);
    }

    bool active() const noexcept override
    {
        return this->state_ &&
               this->state_->active.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<FixtureScheduledState> state_;
};

class FixtureLifecycleScheduler final : public RumbleScheduler
{
public:
    explicit FixtureLifecycleScheduler(ManualScheduler &scheduler)
        : scheduler_(scheduler)
    {
    }

    std::int64_t nowMs() const noexcept override
    {
        return this->scheduler_.nowMs();
    }

    std::unique_ptr<ScheduledTask> scheduleAfter(std::int64_t delayMs,
                                                 Callback callback) override
    {
        if (this->throwSchedules)
            throw std::runtime_error("fixture scheduler unavailable");
        if (this->rejectSchedules)
            return nullptr;
        auto state = std::make_shared<FixtureScheduledState>();
        state->scheduler = &this->scheduler_;
        state->callback = std::move(callback);
        const std::weak_ptr weak = state;
        state->id = this->scheduler_.scheduleAfter(delayMs, [weak] {
            const auto locked = weak.lock();
            if (!locked ||
                !locked->active.exchange(false, std::memory_order_acq_rel))
                return;
            locked->callback();
        });
        this->states_.push_back(state);
        return std::make_unique<FixtureScheduledTask>(std::move(state));
    }

    std::uint64_t randomBelow(std::uint64_t upper) override
    {
        this->randomUpperBounds.push_back(upper);
        return upper == 0
                   ? 0
                   : std::min<std::uint64_t>(this->randomValue, upper - 1);
    }

    void fireIgnoringCancellation(std::size_t index)
    {
        ASSERT_LT(index, this->states_.size());
        ASSERT_TRUE(this->states_[index]->callback);
        this->states_[index]->callback();
    }

    std::size_t scheduledTaskCount() const noexcept
    {
        return this->states_.size();
    }

    std::uint64_t randomValue = 500;
    std::vector<std::uint64_t> randomUpperBounds;
    bool rejectSchedules = false;
    bool throwSchedules = false;

private:
    ManualScheduler &scheduler_;
    std::vector<std::shared_ptr<FixtureScheduledState>> states_;
};

class CompletedCatalogHandle final : public TransportHandle
{
public:
    void cancel() noexcept override
    {
    }

    bool active() const noexcept override
    {
        return false;
    }
};

/// Existing lifecycle fixtures predate the independently tested public emote
/// endpoint. Supply a deterministic empty catalog without weakening their
/// strict page/embed/SSE exchange scripts.
class CatalogAwareFixtureTransport final : public Transport
{
public:
    explicit CatalogAwareFixtureTransport(RumbleFixtureTransport &transport)
        : delegate_(transport)
    {
    }

    std::unique_ptr<TransportHandle> start(
        TransportRequest request, TransportCallbacks callbacks) override
    {
        if (request.url.path() != QStringLiteral("/service.php"))
        {
            return delegate_.start(std::move(request), std::move(callbacks));
        }
        ++catalogRequests;
        EXPECT_EQ(request.url.host(), QStringLiteral("rumble.com"));
        EXPECT_EQ(QUrlQuery(request.url).queryItemValue(QStringLiteral("name")),
                  QStringLiteral("emote.list"));
        EXPECT_FALSE(QUrlQuery(request.url)
                         .queryItemValue(QStringLiteral("chat_id"))
                         .isEmpty());
        EXPECT_EQ(request.expectedMediaType, ExpectedMediaType::Json);
        EXPECT_EQ(request.maxBodyBytes, rumble::MAX_EMOTE_CATALOG_BYTES);
        if (callbacks.onHead)
        {
            callbacks.onHead({
                .status = 200,
                .headers = {{QByteArrayLiteral("Content-Type"),
                             QByteArrayLiteral("application/json")}},
            });
        }
        if (callbacks.onBodyChunk)
        {
            callbacks.onBodyChunk(
                QByteArrayLiteral(R"({"data":{"items":[]}})"));
        }
        if (callbacks.onComplete)
        {
            callbacks.onComplete();
        }
        return std::make_unique<CompletedCatalogHandle>();
    }

    int catalogRequests = 0;

private:
    RumbleFixtureApiTransport delegate_;
};

struct FixtureLifecycle {
    explicit FixtureLifecycle(RumbleFixtureScript script)
        : dispatcher(std::make_shared<OwnerDispatcher>())
        , fixtureTransport(manual, std::move(script))
        , apiTransport(fixtureTransport)
        , scheduler(manual)
        , api(apiTransport,
              [this](std::function<void()> task) {
                  std::ignore = this->manual.scheduleAfter(0, std::move(task));
              })
        , provider(dispatcher)
    {
    }

    std::shared_ptr<RumbleChannel> channel(QString slug)
    {
        auto result = this->provider.getOrCreate(
            RumbleChannelKeyKind::ChannelSlug, std::move(slug));
        EXPECT_TRUE(result);
        return result ? *result : nullptr;
    }

    ManualScheduler manual;
    std::shared_ptr<OwnerDispatcher> dispatcher;
    RumbleFixtureTransport fixtureTransport;
    CatalogAwareFixtureTransport apiTransport;
    FixtureLifecycleScheduler scheduler;
    RumbleApi api;
    RumbleChannelProvider provider;
};

std::string sse(std::string json)
{
    return "data: " + std::move(json) + "\n\n";
}

RumbleFixtureExchange streamExchange(
    std::string label, int status,
    std::vector<RumbleFixtureHeader> headers = {},
    std::vector<RumbleFixtureChunk> chunks = {},
    RumbleFixtureTerminal terminal = RumbleFixtureTerminal::Complete,
    std::int64_t terminalAfterMs = 0, std::string streamId = "1001")
{
    if (headers.empty() && status == 200)
    {
        headers.push_back({"Content-Type", "text/event-stream"});
    }
    return {
        .label = std::move(label),
        .expectedRequest =
            {
                .method = "GET",
                .target = "/chat/api/chat/" + streamId + "/stream",
                .headers =
                    {
                        {"Accept", "text/event-stream"},
                        {"Cache-Control", "no-cache"},
                    },
            },
        .response =
            {
                .status = status,
                .headers = std::move(headers),
            },
        .chunks = std::move(chunks),
        .terminal = terminal,
        .terminalAfterMs = terminalAfterMs,
    };
}

RumbleFixtureScript directStreamScript(
    std::vector<RumbleFixtureExchange> exchanges)
{
    return {
        .name = "direct-stream-lifecycle",
        .exchanges = std::move(exchanges),
    };
}

class InactiveTransportHandle final : public TransportHandle
{
public:
    void cancel() noexcept override
    {
    }
    bool active() const noexcept override
    {
        return false;
    }
};

class ImmediateFailureTransport final : public Transport
{
public:
    explicit ImmediateFailureTransport(TransportFailure failure)
        : failure_(failure)
    {
    }

    std::unique_ptr<TransportHandle> start(
        TransportRequest, TransportCallbacks callbacks) override
    {
        callbacks.onHead({
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
        });
        callbacks.onFailure(this->failure_);
        return std::make_unique<InactiveTransportHandle>();
    }

private:
    TransportFailure failure_;
};

}  // namespace

TEST(RumbleLifecycle, ResolvesConnectsPublishesAndBacksOffOnDisconnect)
{
    MockApplication app;
    FixtureLifecycle fixture(
        loadRumbleFixtureScenario(QStringLiteral("live-session")));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    auto otherChannel = fixture.channel(QStringLiteral("other-channel"));
    ASSERT_TRUE(otherChannel);
    const std::array specs{
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/c/fixture-channel"),
        },
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/c/other-channel"),
        },
    };
    const std::array<ChannelPtr, 2> children{channel, otherChannel};
    std::size_t childIndex = 0;
    MultiChannel multi(specs,
                       MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
                       [&](const MultiChannel::Spec &) -> ChannelPtr {
                           return children.at(childIndex++);
                       });
    EXPECT_EQ(multi.getDisplayName(),
              QStringLiteral("fixture-channel, other-channel"));
    std::vector<RumbleChannelState> states;
    std::ignore = channel->stateChanged.connect([&](auto, auto current) {
        states.push_back(current);
    });

    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    fixture.manual.advanceBy(25);

    EXPECT_EQ(channel->state(), RumbleChannelState::Backoff);
    EXPECT_NE(std::ranges::find(states, RumbleChannelState::Connected),
              states.end());
    const auto lifecycle = channel->lifecycleSnapshot();
    EXPECT_EQ(lifecycle.state, RumbleChannelState::Backoff);
    EXPECT_EQ(lifecycle.metadata.consecutiveFailures, 1U);
    EXPECT_TRUE(lifecycle.metadata.deadlineAtMs);
    ASSERT_TRUE(channel->metadata());
    ASSERT_TRUE(channel->metadata()->channelSlug());
    EXPECT_EQ(channel->metadata()->channelSlug()->value(),
              QStringLiteral("fixture-channel"));
    EXPECT_EQ(channel->metadata()->displayName(),
              QStringLiteral("fixture-channel"));
    EXPECT_EQ(channel->metadata()->streamTitle(),
              QStringLiteral("Fixture Stream"));
    EXPECT_EQ(channel->streamTitle(), QStringLiteral("Fixture Stream"));
    EXPECT_EQ(channel->getDisplayName(), QStringLiteral("fixture-channel"));
    EXPECT_NE(channel->getDisplayName(), QStringLiteral("Fixture Stream"));
    EXPECT_EQ(multi.getDisplayName(),
              QStringLiteral("fixture-channel, other-channel"));
    EXPECT_FALSE(
        multi.getDisplayName().contains(QStringLiteral("Fixture Stream")));
    EXPECT_EQ(channel->getMessageSnapshot().size(), 2U);
    EXPECT_EQ(fixture.apiTransport.catalogRequests, 1);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
}

TEST(RumbleLifecycle, CleanStreamEndRevalidatesLocatorBeforeConfirmingOffline)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    ASSERT_EQ(script.exchanges.size(), 3U);
    script.exchanges.resize(2);
    script.exchanges.push_back(streamExchange(
        "clean-end-stream", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Complete, 25));
    auto offline = loadRumbleFixtureScenario(QStringLiteral("offline-page"));
    ASSERT_EQ(offline.exchanges.size(), 1U);
    auto offlineExchange = offline.exchanges.front();
    offlineExchange.expectedRequest.target = "/c/fixture-channel/live/";
    script.exchanges.push_back(std::move(offlineExchange));
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    std::vector<bool> liveValues;
    std::ignore = channel->liveStatusChanged.connect([&] {
        liveValues.push_back(channel->isLive());
    });

    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    ASSERT_TRUE(channel->isLive());

    fixture.manual.advanceBy(25);
    ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
    EXPECT_TRUE(channel->isLive());
    EXPECT_EQ(channel->lifecycleMetadata().retryCause,
              RumbleRetryCause::StreamEnded);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);

    fixture.manual.advanceBy(499);
    EXPECT_EQ(channel->state(), RumbleChannelState::Backoff);
    EXPECT_TRUE(channel->isLive());
    fixture.manual.advanceBy(1);
    EXPECT_EQ(channel->state(), RumbleChannelState::Offline);
    EXPECT_FALSE(channel->isLive());
    EXPECT_EQ(liveValues, (std::vector<bool>{true, false}));
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, CleanStreamEndWithLiveRevalidationNeverFlashesOffline)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    ASSERT_EQ(script.exchanges.size(), 3U);
    const auto page = script.exchanges[0];
    const auto embed = script.exchanges[1];
    script.exchanges.resize(2);
    script.exchanges.push_back(streamExchange(
        "clean-end-stream", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Complete, 25));
    script.exchanges.push_back(page);
    script.exchanges.push_back(embed);
    script.exchanges.push_back(streamExchange(
        "revalidated-stream", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000));
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    std::vector<bool> liveValues;
    std::ignore = channel->liveStatusChanged.connect([&] {
        liveValues.push_back(channel->isLive());
    });

    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    fixture.manual.advanceBy(25);
    ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
    ASSERT_TRUE(channel->isLive());
    fixture.manual.advanceBy(500);

    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    EXPECT_TRUE(channel->isLive());
    EXPECT_EQ(liveValues, (std::vector<bool>{true}));
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, GoneStreamEndpointRevalidatesLocatorBeforeOffline)
{
    for (const auto status : {404, 410})
    {
        SCOPED_TRACE(status);
        MockApplication app;
        auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
        ASSERT_EQ(script.exchanges.size(), 3U);
        script.exchanges.resize(2);
        script.exchanges.push_back(streamExchange(
            "connected-stream", 200, {},
            {{.bytes = sse(
                  R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
            RumbleFixtureTerminal::Disconnect, 25));
        script.exchanges.push_back(streamExchange("gone-stream", status));
        auto offline =
            loadRumbleFixtureScenario(QStringLiteral("offline-page"));
        ASSERT_EQ(offline.exchanges.size(), 1U);
        auto offlineExchange = offline.exchanges.front();
        offlineExchange.expectedRequest.target = "/c/fixture-channel/live/";
        script.exchanges.push_back(std::move(offlineExchange));
        FixtureLifecycle fixture(std::move(script));
        auto channel = fixture.channel(QStringLiteral("fixture-channel"));
        ASSERT_TRUE(channel);

        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher,
                                    QStringLiteral("fixture-channel"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();
        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
        ASSERT_TRUE(channel->isLive());

        fixture.manual.advanceBy(25);
        ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
        ASSERT_TRUE(channel->isLive());
        fixture.manual.advanceBy(500);
        ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
        EXPECT_TRUE(channel->isLive());
        EXPECT_EQ(channel->lifecycleMetadata().retryCause,
                  RumbleRetryCause::StreamEnded);
        fixture.manual.advanceBy(500);

        EXPECT_EQ(channel->state(), RumbleChannelState::Offline);
        EXPECT_FALSE(channel->isLive());
        EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
    }
}

TEST(RumbleLifecycle, OfflineRecheckRestoresLiveAfterEndedStream)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    ASSERT_EQ(script.exchanges.size(), 3U);
    const auto page = script.exchanges[0];
    const auto embed = script.exchanges[1];
    script.exchanges.resize(2);
    script.exchanges.push_back(streamExchange(
        "clean-end-stream", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Complete, 25));
    auto offline = loadRumbleFixtureScenario(QStringLiteral("offline-page"));
    ASSERT_EQ(offline.exchanges.size(), 1U);
    auto offlineExchange = offline.exchanges.front();
    offlineExchange.expectedRequest.target = "/c/fixture-channel/live/";
    script.exchanges.push_back(std::move(offlineExchange));
    script.exchanges.push_back(page);
    script.exchanges.push_back(embed);
    script.exchanges.push_back(streamExchange(
        "replacement-stream", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000));
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    std::vector<bool> liveValues;
    std::ignore = channel->liveStatusChanged.connect([&] {
        liveValues.push_back(channel->isLive());
    });

    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_TRUE(channel->isLive());
    const auto persistentName = channel->getName();
    const auto displayName = channel->getDisplayName();
    fixture.manual.advanceBy(25);
    fixture.manual.advanceBy(500);
    ASSERT_EQ(channel->state(), RumbleChannelState::Offline);
    ASSERT_FALSE(channel->isLive());

    fixture.manual.advanceBy(29999);
    EXPECT_EQ(channel->state(), RumbleChannelState::Offline);
    fixture.manual.advanceBy(1);
    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    EXPECT_TRUE(channel->isLive());
    EXPECT_EQ(channel->getName(), persistentName);
    EXPECT_EQ(channel->getDisplayName(), displayName);
    EXPECT_EQ(liveValues, (std::vector<bool>{true, false, true}));
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, StaleLiveResolutionCannotLoopEndedStreamForever)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    ASSERT_EQ(script.exchanges.size(), 3U);
    const auto page = script.exchanges[0];
    const auto embed = script.exchanges[1];
    script.exchanges.resize(2);
    auto appendCleanEnd = [&] {
        script.exchanges.push_back(streamExchange(
            "immediate-clean-end", 200, {},
            {{.bytes = sse(
                  R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
            RumbleFixtureTerminal::Complete, 25));
    };
    appendCleanEnd();
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        script.exchanges.push_back(page);
        script.exchanges.push_back(embed);
        appendCleanEnd();
    }

    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    std::vector<bool> liveValues;
    std::ignore = channel->liveStatusChanged.connect([&] {
        liveValues.push_back(channel->isLive());
    });

    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        fixture.manual.advanceBy(25);
        ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
        fixture.manual.advanceBy(500);
        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    }
    fixture.manual.advanceBy(25);

    EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
    EXPECT_TRUE(channel->isLive());
    EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 3U);
    EXPECT_EQ(channel->lifecycleMetadata().retryCause,
              RumbleRetryCause::StreamEnded);
    ASSERT_TRUE(channel->failure());
    EXPECT_EQ(channel->failure()->code(), RumbleFailureCode::RetryExhausted);
    EXPECT_EQ(liveValues, (std::vector<bool>{true}));
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, StableOrAdvancingStreamResetsEndedFailureBound)
{
    enum class ResetEvidence { StableConnection, PostInitEvent };
    for (const auto evidence :
         {ResetEvidence::StableConnection, ResetEvidence::PostInitEvent})
    {
        SCOPED_TRACE(evidence == ResetEvidence::StableConnection
                         ? "stable-connection"
                         : "post-init-event");
        MockApplication app;
        auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
        ASSERT_EQ(script.exchanges.size(), 3U);
        const auto page = script.exchanges[0];
        const auto embed = script.exchanges[1];
        script.exchanges.resize(2);
        auto cleanEnd = [](std::string label, std::int64_t terminalAfterMs,
                           bool includePostInitEvent = false) {
            auto bytes = sse(
                R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})");
            if (includePostInitEvent)
            {
                bytes += sse(R"({"type":"messages","data":{"messages":[]}})");
            }
            return streamExchange(
                std::move(label), 200, {}, {{.bytes = std::move(bytes)}},
                RumbleFixtureTerminal::Complete, terminalAfterMs);
        };
        script.exchanges.push_back(cleanEnd("first-clean-end", 25));
        script.exchanges.push_back(page);
        script.exchanges.push_back(embed);
        const bool providerEvent = evidence == ResetEvidence::PostInitEvent;
        const auto resetDelay = providerEvent ? 25 : 10'000;
        script.exchanges.push_back(
            cleanEnd("resetting-clean-end", resetDelay, providerEvent));
        script.exchanges.push_back(page);
        script.exchanges.push_back(embed);
        script.exchanges.push_back(cleanEnd("bounded-clean-end", 25));
        auto offline =
            loadRumbleFixtureScenario(QStringLiteral("offline-page"));
        ASSERT_EQ(offline.exchanges.size(), 1U);
        auto offlineExchange = offline.exchanges.front();
        offlineExchange.expectedRequest.target = "/c/fixture-channel/live/";
        script.exchanges.push_back(std::move(offlineExchange));

        FixtureLifecycle fixture(std::move(script));
        auto channel = fixture.channel(QStringLiteral("fixture-channel"));
        ASSERT_TRUE(channel);
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher,
                                    QStringLiteral("fixture-channel"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();
        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);

        fixture.manual.advanceBy(25);
        ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
        fixture.manual.advanceBy(500);
        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
        fixture.manual.advanceBy(resetDelay);
        ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
        fixture.manual.advanceBy(500);
        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
        fixture.manual.advanceBy(25);

        // Without the reset evidence, this would be the third consecutive
        // immediately-ended resolution and would already be Failed.
        EXPECT_EQ(channel->state(), RumbleChannelState::Backoff);
        EXPECT_TRUE(channel->isLive());
        fixture.manual.advanceBy(500);
        EXPECT_EQ(channel->state(), RumbleChannelState::Offline);
        EXPECT_FALSE(channel->isLive());
        EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
    }
}

TEST(RumbleLifecycle, OfflineRechecksAtThirtySecondLowerBound)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("offline-page"));
    ASSERT_EQ(script.exchanges.size(), 1U);
    script.exchanges.push_back(script.exchanges.front());
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-offline"));
    ASSERT_TRUE(channel);
    RumbleConnection::Options options;
    options.offlineRecheckMs = 0;  // clamped to the contract minimum
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-offline"), options);
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Offline);
    auto snapshot = channel->lifecycleSnapshot();
    ASSERT_TRUE(snapshot.metadata.deadlineAtMs);
    EXPECT_EQ(*snapshot.metadata.deadlineAtMs, 30000);

    fixture.manual.advanceBy(29999);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);
    fixture.manual.advanceBy(1);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
    EXPECT_EQ(channel->state(), RumbleChannelState::Offline);
}

TEST(RumbleLifecycle, AccessInterstitialAndSchemaUseDistinctOperatorText)
{
    struct Case {
        std::string slug;
        std::string body;
        RumbleFailureCategory category;
        RumbleFailureCode code;
        RumbleOperatorText text;
        RumbleRetryCause cause;
        QString safeText;
    };
    const std::vector<Case> cases = {
        {
            .slug = "access",
            .body = "<html><title>Just a moment...</title>"
                    "<form id=\"challenge-form\"></form></html>",
            .category = RumbleFailureCategory::Transport,
            .code = RumbleFailureCode::RejectedHttp,
            .text = RumbleOperatorText::ServiceRejectedRequest,
            .cause = RumbleRetryCause::HttpFailure,
            .safeText = QStringLiteral("The service rejected the request."),
        },
        {
            .slug = "schema",
            .body = "<html><title/>Malformed contract</html>",
            .category = RumbleFailureCategory::Protocol,
            .code = RumbleFailureCode::MalformedResponse,
            .text = RumbleOperatorText::ResponseContractChanged,
            .cause = RumbleRetryCause::ProtocolFailure,
            .safeText = QStringLiteral("Rumble sent an unexpected response."),
        },
    };

    for (const auto &test : cases)
    {
        SCOPED_TRACE(test.slug);
        MockApplication app;
        auto script = loadRumbleFixtureScenario(QStringLiteral("offline-page"));
        ASSERT_EQ(script.exchanges.size(), 1U);
        script.exchanges[0].expectedRequest.target =
            "/c/" + test.slug + "/live/";
        script.exchanges[0].chunks = {{.bytes = test.body}};
        FixtureLifecycle fixture(std::move(script));
        auto channel = fixture.channel(QString::fromStdString(test.slug));
        ASSERT_TRUE(channel);
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher,
                                    QString::fromStdString(test.slug));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();

        EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
        ASSERT_TRUE(channel->failure());
        EXPECT_EQ(channel->failure()->category(), test.category);
        EXPECT_EQ(channel->failure()->code(), test.code);
        EXPECT_EQ(channel->failure()->text(), test.text);
        EXPECT_FALSE(channel->failure()->diagnosticCode().isEmpty());
        EXPECT_EQ(channel->failure()->operatorSafeText(), test.safeText);
        EXPECT_EQ(channel->lifecycleMetadata().retryCause, test.cause);
    }

    const RumbleFailure unsafe(RumbleFailureCategory::Protocol,
                               RumbleFailureCode::MalformedResponse,
                               RumbleOperatorText::ResponseContractChanged,
                               QStringLiteral("secret value must not escape"));
    EXPECT_TRUE(unsafe.diagnosticCode().isEmpty());
    EXPECT_EQ(unsafe.operatorSafeText(),
              QStringLiteral("Rumble sent an unexpected response."));
}

TEST(RumbleLifecycle, StopBeforeStartIsTerminalAndCreatesNoWork)
{
    MockApplication app;
    FixtureLifecycle fixture(
        loadRumbleFixtureScenario(QStringLiteral("offline-page")));
    auto channel = fixture.channel(QStringLiteral("stop-before-start"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("stop-before-start"));

    connection.stop();

    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_FALSE(connection.start());
    connection.retry();
    connection.restart(QStringLiteral("replacement"));
    fixture.dispatcher->runAll();
    fixture.manual.runReady();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);
    EXPECT_EQ(fixture.scheduler.scheduledTaskCount(), 0U);
}

TEST(RumbleLifecycle, ProviderShutdownCancelsPendingOfflineTimer)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("offline-page"));
    script.exchanges.push_back(script.exchanges.front());
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-offline"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-offline"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Offline);
    ASSERT_EQ(fixture.scheduler.scheduledTaskCount(), 1U);
    ASSERT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);

    fixture.provider.shutdown();

    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
    fixture.scheduler.fireIgnoringCancellation(0);
    fixture.manual.runReady();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);
}

TEST(RumbleReconnect, ReentrantFirstAppendDoesNotLoseLaterMessageId)
{
    MockApplication app;
    auto first = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    auto second = first;
    ASSERT_EQ(first.exchanges.size(), 3U);
    first.exchanges[2].chunks = {{
        .bytes =
            sse(R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})") +
            sse(R"({"type":"messages","data":{"messages":[{"id":"m1","user_id":"u","channel_id":"c","text":"one","created_on":"2026-01-01T00:00:01Z"},{"id":"m2","user_id":"u","channel_id":"c","text":"two","created_on":"2026-01-01T00:00:02Z"}]}})"),
    }};
    second.exchanges[2].chunks = {{
        .bytes = sse(
            R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[{"id":"m2","user_id":"u","channel_id":"c","text":"two","created_on":"2026-01-01T00:00:02Z"}]}})"),
    }};
    first.exchanges.insert(first.exchanges.end(), second.exchanges.begin(),
                           second.exchanges.end());

    FixtureLifecycle fixture(std::move(first));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    bool reentered = false;
    std::ignore = channel->messageAppended.connect([&](auto &message, auto) {
        if (!reentered && message->id == QStringLiteral("m1"))
        {
            reentered = true;
            channel->reconnect();
        }
    });
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();

    EXPECT_TRUE(reentered);
    const auto messages = channel->getMessageSnapshot();
    EXPECT_NE(std::ranges::find_if(messages,
                                   [](const auto &message) {
                                       return message->id ==
                                              QStringLiteral("m1");
                                   }),
              messages.end());
    EXPECT_NE(std::ranges::find_if(messages,
                                   [](const auto &message) {
                                       return message->id ==
                                              QStringLiteral("m2");
                                   }),
              messages.end());
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleReconnect, DelegateInstallationReentrancyQueuesOneLifecycle)
{
    MockApplication app;
    FixtureLifecycle fixture(
        loadRumbleFixtureScenario(QStringLiteral("live-session")));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    bool reentered = false;
    std::ignore = channel->reconnectAvailabilityChanged.connect([&] {
        if (!reentered && channel->canReconnect())
        {
            reentered = true;
            channel->reconnect();
        }
    });
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    fixture.manual.advanceBy(25);

    EXPECT_TRUE(reentered);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
    EXPECT_EQ(channel->getMessageSnapshot().size(), 2U);
}

TEST(RumbleReconnect, MetadataAndConnectingReentrancyStopOldContinuation)
{
    for (const bool onMetadata : {true, false})
    {
        SCOPED_TRACE(onMetadata ? "metadata" : "connecting-state");
        MockApplication app;
        const auto complete =
            loadRumbleFixtureScenario(QStringLiteral("live-session"));
        ASSERT_EQ(complete.exchanges.size(), 3U);
        RumbleFixtureScript script{
            .name = "reentrant-resolution-publication",
            .exchanges =
                {
                    complete.exchanges[0],
                    complete.exchanges[1],
                    complete.exchanges[0],
                    complete.exchanges[1],
                    complete.exchanges[2],
                },
        };
        FixtureLifecycle fixture(std::move(script));
        auto channel = fixture.channel(QStringLiteral("fixture-channel"));
        ASSERT_TRUE(channel);
        bool reentered = false;
        std::ignore = channel->locatorChanged.connect([&] {
            if (onMetadata && !reentered)
            {
                reentered = true;
                channel->reconnect();
            }
        });
        std::ignore = channel->stateChanged.connect([&](auto, auto state) {
            if (!onMetadata && !reentered &&
                state == RumbleChannelState::Connecting)
            {
                reentered = true;
                channel->reconnect();
            }
        });
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher,
                                    QStringLiteral("fixture-channel"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();
        fixture.manual.advanceBy(25);

        EXPECT_TRUE(reentered);
        EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
        EXPECT_EQ(channel->getMessageSnapshot().size(), 2U);
    }
}

TEST(RumbleReconnect, HistorySignalReentrancyLeavesTailForNextGeneration)
{
    MockApplication app;
    const auto complete =
        loadRumbleFixtureScenario(QStringLiteral("live-session"));
    ASSERT_EQ(complete.exchanges.size(), 3U);
    RumbleFixtureScript script{
        .name = "reentrant-history-publication",
        .exchanges =
            {
                complete.exchanges[0],
                complete.exchanges[1],
                complete.exchanges[2],
                complete.exchanges[0],
                complete.exchanges[1],
                complete.exchanges[2],
            },
    };
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    bool reentered = false;
    std::ignore = channel->filledInMessages.connect([&](const auto &) {
        if (!reentered)
        {
            reentered = true;
            channel->reconnect();
        }
    });
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    fixture.manual.advanceBy(50);

    EXPECT_TRUE(reentered);
    const auto messages = channel->getMessageSnapshot();
    EXPECT_NE(std::ranges::find_if(messages,
                                   [](const auto &message) {
                                       return message->id ==
                                              QStringLiteral("m2");
                                   }),
              messages.end());
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleReconnect, LiveLifecycleAndPinSignalsCannotPublishOldTail)
{
    for (const int boundary : {0, 1, 2})
    {
        SCOPED_TRACE(boundary);
        MockApplication app;
        auto first = streamExchange(
            "first", 200, {},
            {{.bytes =
                  sse(R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})") +
                  sse(R"({"type":"pin_message","data":{"message":{"id":"pin","user_id":"u","channel_id":"c","text":"pin","created_on":"2026-01-01T00:00:00Z"}}})") +
                  sse(R"({"type":"messages","data":{"messages":[{"id":"old-tail","user_id":"u","channel_id":"c","text":"old","created_on":"2026-01-01T00:00:01Z"}]}})")}},
            RumbleFixtureTerminal::Disconnect);
        if (boundary == 1)
        {
            first.chunks.clear();
            first.terminal = RumbleFixtureTerminal::Complete;
        }
        auto second = streamExchange(
            "second", 200, {},
            {{.bytes = sse(
                  R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[{"id":"new","user_id":"u","channel_id":"c","text":"new","created_on":"2026-01-01T00:00:02Z"}]}})")}},
            RumbleFixtureTerminal::Disconnect, 10000);
        FixtureLifecycle fixture(
            directStreamScript({std::move(first), std::move(second)}));
        auto channel = fixture.channel(QStringLiteral("signal-boundary"));
        ASSERT_TRUE(channel);
        bool reentered = false;
        std::ignore = channel->liveStatusChanged.connect([&] {
            if (boundary == 0 && !reentered && channel->isLive())
            {
                reentered = true;
                channel->reconnect();
            }
        });
        std::ignore = channel->lifecycleMetadataChanged.connect([&] {
            if (boundary == 1 && !reentered &&
                channel->state() == RumbleChannelState::Backoff)
            {
                reentered = true;
                channel->reconnect();
            }
        });
        std::ignore = channel->pinnedMessageChanged.connect([&] {
            if (boundary == 2 && !reentered)
            {
                reentered = true;
                channel->reconnect();
            }
        });
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher, QStringLiteral("1001"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();

        EXPECT_TRUE(reentered);
        const auto messages = channel->getMessageSnapshot();
        EXPECT_EQ(std::ranges::count_if(messages,
                                        [](const auto &message) {
                                            return message->id ==
                                                   QStringLiteral("old-tail");
                                        }),
                  0);
        EXPECT_EQ(std::ranges::count_if(messages,
                                        [](const auto &message) {
                                            return message->id ==
                                                   QStringLiteral("new");
                                        }),
                  1);
        EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
    }
}

TEST(RumbleReconnect, SchedulerFailureFromSignalBoundaryIsDeferred)
{
    MockApplication app;
    FixtureLifecycle fixture(directStreamScript({streamExchange(
        "connected", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000)}));
    auto channel = fixture.channel(QStringLiteral("scheduler-signal-boundary"));
    ASSERT_TRUE(channel);
    bool reentered = false;
    std::ignore = channel->liveStatusChanged.connect([&] {
        if (!reentered && channel->isLive())
        {
            reentered = true;
            fixture.scheduler.rejectSchedules = true;
            channel->reconnect();
        }
    });
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();

    ASSERT_TRUE(reentered);
    EXPECT_NE(channel->state(), RumbleChannelState::Failed);
    fixture.dispatcher->runAll();
    EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
    EXPECT_EQ(channel->lifecycleMetadata().retryCause,
              RumbleRetryCause::SchedulerUnavailable);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
}

TEST(RumbleReconnect, AutomaticStreamRetryDeduplicatesBootstrapOverlap)
{
    MockApplication app;
    auto first = streamExchange(
        "disconnect", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[{"id":"m1","user_id":"u","channel_id":"c","text":"one","created_on":"2026-01-01T00:00:00Z"}]}})")}},
        RumbleFixtureTerminal::Disconnect);
    auto second = streamExchange(
        "reconnect", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[{"id":"m1","user_id":"u","channel_id":"c","text":"one","created_on":"2026-01-01T00:00:00Z"},{"id":"m2","user_id":"u","channel_id":"c","text":"two","created_on":"2026-01-01T00:00:01Z"}]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000);
    FixtureLifecycle fixture(
        directStreamScript({std::move(first), std::move(second)}));
    auto channel = fixture.channel(QStringLiteral("auto-reconnect"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
    EXPECT_EQ(channel->getMessageSnapshot().size(), 1U);

    fixture.manual.advanceBy(500);
    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 0U);
    const auto messages = channel->getMessageSnapshot();
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0]->id, QStringLiteral("m1"));
    EXPECT_EQ(messages[1]->id, QStringLiteral("m2"));
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);
}

TEST(RumbleLifecycle, NumericEmbedChannelIdentityIsNeverPersistedAsSlug)
{
    MockApplication app;
    RumbleFixtureScript script{
        .name = "numeric-embed-channel-id",
        .exchanges =
            {
                {
                    .label = "embed",
                    .expectedRequest =
                        {
                            .method = "GET",
                            .target =
                                "/embedJS/u3/?request=video&ver=2&v=vfixture",
                            .headers = {{"Accept", "application/json"}},
                        },
                    .response =
                        {
                            .status = 200,
                            .headers = {{"Content-Type", "application/json"}},
                        },
                    .chunks = {{
                        .bytes =
                            R"({"vid":1001,"title":"Video","channel_id":777,"channel_title":"Display"})",
                    }},
                },
                {
                    .label = "offline-stream",
                    .expectedRequest =
                        {
                            .method = "GET",
                            .target = "/chat/api/chat/1001/stream",
                            .headers =
                                {
                                    {"Accept", "text/event-stream"},
                                    {"Cache-Control", "no-cache"},
                                },
                        },
                    .response =
                        {
                            .status = 204,
                        },
                },
            },
    };
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("vfixture"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("vfixture"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();

    ASSERT_TRUE(channel->metadata());
    EXPECT_FALSE(channel->metadata()->channelSlug());
    ASSERT_TRUE(channel->metadata()->embedId());
    EXPECT_EQ(channel->metadata()->embedId()->value(),
              QStringLiteral("vfixture"));
    ASSERT_TRUE(channel->metadata()->streamId());
    EXPECT_EQ(channel->metadata()->streamId()->value(), QStringLiteral("1001"));
    EXPECT_EQ(channel->state(), RumbleChannelState::Offline);
}

TEST(RumbleLifecycle, HttpFailuresUseTypedRetryAndRevalidationStates)
{
    struct Case {
        int status;
        RumbleChannelState state;
        bool retryable;
        RumbleRetryCause cause;
    };
    for (const auto test : std::vector<Case>{
             {408, RumbleChannelState::Backoff, true,
              RumbleRetryCause::HttpFailure},
             {503, RumbleChannelState::Backoff, true,
              RumbleRetryCause::HttpFailure},
             {404, RumbleChannelState::Backoff, true,
              RumbleRetryCause::StreamEnded},
             {410, RumbleChannelState::Backoff, true,
              RumbleRetryCause::StreamEnded},
         })
    {
        SCOPED_TRACE(test.status);
        MockApplication app;
        FixtureLifecycle fixture(
            directStreamScript({streamExchange("http", test.status)}));
        auto channel = fixture.channel(QStringLiteral("http-case"));
        ASSERT_TRUE(channel);
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher, QStringLiteral("1001"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();
        EXPECT_EQ(channel->state(), test.state);
        const auto snapshot = channel->lifecycleSnapshot();
        EXPECT_EQ(snapshot.metadata.consecutiveFailures, 1U);
        if (test.retryable)
        {
            EXPECT_TRUE(snapshot.metadata.deadlineAtMs);
            EXPECT_EQ(snapshot.metadata.retryCause, test.cause);
        }
        else
        {
            EXPECT_FALSE(snapshot.metadata.deadlineAtMs);
            ASSERT_TRUE(channel->failure());
        }
    }
}

TEST(RumbleLifecycle, TimeoutUsesTypedBackoffCause)
{
    MockApplication app;
    ManualScheduler manual;
    ImmediateFailureTransport transport(TransportFailure::Timeout);
    FixtureLifecycleScheduler scheduler(manual);
    auto dispatcher = std::make_shared<OwnerDispatcher>();
    RumbleApi api(transport, [&](std::function<void()> task) {
        std::ignore = manual.scheduleAfter(0, std::move(task));
    });
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("timeout"));
    ASSERT_TRUE(created);
    auto channel = *created;
    RumbleConnection connection(channel, api, scheduler, dispatcher,
                                QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    manual.runReady();

    EXPECT_EQ(channel->state(), RumbleChannelState::Backoff);
    EXPECT_EQ(channel->lifecycleMetadata().retryCause,
              RumbleRetryCause::Timeout);
    EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 1U);
}

TEST(RumbleLifecycle, BodyLimitUsesDistinctDiagnosticText)
{
    MockApplication app;
    ManualScheduler manual;
    ImmediateFailureTransport transport(TransportFailure::BodyLimit);
    FixtureLifecycleScheduler scheduler(manual);
    auto dispatcher = std::make_shared<OwnerDispatcher>();
    RumbleApi api(transport, [&](std::function<void()> task) {
        std::ignore = manual.scheduleAfter(0, std::move(task));
    });
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("large-page"));
    ASSERT_TRUE(created);
    auto channel = *created;
    RumbleConnection connection(channel, api, scheduler, dispatcher,
                                QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    manual.runReady();

    EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
    ASSERT_TRUE(channel->failure());
    EXPECT_EQ(channel->failure()->text(),
              RumbleOperatorText::ResponseLimitExceeded);
    EXPECT_EQ(channel->failure()->diagnosticCode(),
              QStringLiteral("sse_body_limit"));
    EXPECT_EQ(channel->failure()->operatorSafeText(),
              QStringLiteral(
                  "Rumble sent a response Chatterino could not process."));
}

TEST(RumbleLifecycle, NonRetryableSchemaAfterInitStagesThroughErrorState)
{
    MockApplication app;
    FixtureLifecycle fixture(directStreamScript({streamExchange(
        "schema-after-init", 200, {},
        {
            {.bytes = sse(
                 R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")},
            {.afterMs = 100, .bytes = std::string(1, '\0')},
        },
        RumbleFixtureTerminal::Disconnect, 10000)}));
    auto channel = fixture.channel(QStringLiteral("schema"));
    ASSERT_TRUE(channel);
    std::vector<RumbleChannelState> states;
    std::ignore = channel->stateChanged.connect([&](auto, auto state) {
        states.push_back(state);
    });
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    fixture.manual.advanceBy(100);

    EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
    EXPECT_EQ(channel->lifecycleMetadata().retryCause,
              RumbleRetryCause::ProtocolFailure);
    EXPECT_NE(std::ranges::find(states, RumbleChannelState::Backoff),
              states.end());
}

TEST(RumbleLifecycle, RateLimitHonorsRetryAfterAndInvalidValueUsesJitter)
{
    {
        MockApplication app;
        auto limited =
            streamExchange("rate-limited", 429, {{"Retry-After", "3"}});
        auto connected = streamExchange(
            "connected", 200, {},
            {{.bytes = sse(
                  R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
            RumbleFixtureTerminal::Disconnect, 10000);
        FixtureLifecycle fixture(
            directStreamScript({std::move(limited), std::move(connected)}));
        auto channel = fixture.channel(QStringLiteral("rate-limit"));
        ASSERT_TRUE(channel);
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher, QStringLiteral("1001"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();
        auto snapshot = channel->lifecycleSnapshot();
        ASSERT_EQ(snapshot.state, RumbleChannelState::Backoff);
        EXPECT_TRUE(snapshot.metadata.rateLimited);
        EXPECT_EQ(snapshot.metadata.retryCause, RumbleRetryCause::RateLimited);
        ASSERT_TRUE(snapshot.metadata.deadlineAtMs);
        EXPECT_EQ(*snapshot.metadata.deadlineAtMs, 3000);
        EXPECT_TRUE(fixture.scheduler.randomUpperBounds.empty());

        fixture.manual.advanceBy(2999);
        EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);
        fixture.manual.advanceBy(1);
        EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
        EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 0U);
    }

    {
        MockApplication app;
        FixtureLifecycle fixture(directStreamScript({streamExchange(
            "invalid-retry-after", 429, {{"Retry-After", "not-a-delay"}})}));
        fixture.scheduler.randomValue = 777;
        auto channel = fixture.channel(QStringLiteral("invalid-rate-limit"));
        ASSERT_TRUE(channel);
        RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                    fixture.dispatcher, QStringLiteral("1001"));
        ASSERT_TRUE(connection.start());
        fixture.manual.runReady();
        const auto snapshot = channel->lifecycleSnapshot();
        ASSERT_EQ(snapshot.state, RumbleChannelState::Backoff);
        ASSERT_EQ(fixture.scheduler.randomUpperBounds,
                  std::vector<std::uint64_t>{1001});
        ASSERT_TRUE(snapshot.metadata.deadlineAtMs);
        EXPECT_EQ(*snapshot.metadata.deadlineAtMs, 777);
    }
}

TEST(RumbleLifecycle, FullJitterExponentAndCapAreDeterministic)
{
    MockApplication app;
    std::vector<RumbleFixtureExchange> failures;
    for (int index = 0; index < 8; ++index)
    {
        failures.push_back(
            streamExchange("empty-" + std::to_string(index), 200));
    }
    FixtureLifecycle fixture(directStreamScript(std::move(failures)));
    fixture.scheduler.randomValue = 0;
    auto channel = fixture.channel(QStringLiteral("jitter-cap"));
    ASSERT_TRUE(channel);
    RumbleConnection::Options options;
    options.maximumConsecutiveFailures = 8;
    options.maximumBackoffMs = 30 * 1000;
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"),
                                options);
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();

    EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
    EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 8U);
    EXPECT_EQ(fixture.scheduler.randomUpperBounds,
              (std::vector<std::uint64_t>{1001, 2001, 4001, 8001, 16001, 30001,
                                          30001}));
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, ThreeFailuresStopAndManualRetryReResolvesThenResets)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    ASSERT_EQ(script.exchanges.size(), 3U);
    const auto page = script.exchanges[0];
    const auto embed = script.exchanges[1];
    script.exchanges.resize(2);
    for (int index = 0; index < 3; ++index)
    {
        script.exchanges.push_back(
            streamExchange("missing-init-" + std::to_string(index), 200));
        if (index < 2)
        {
            script.exchanges.push_back(page);
            script.exchanges.push_back(embed);
        }
    }
    script.exchanges.push_back(page);
    script.exchanges.push_back(embed);
    script.exchanges.push_back(streamExchange(
        "manual-retry-success", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000));

    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-channel"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-channel"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
    fixture.manual.advanceBy(500);
    ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
    fixture.manual.advanceBy(500);
    ASSERT_EQ(channel->state(), RumbleChannelState::Failed);
    EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 3U);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 3U);

    connection.retry();
    fixture.manual.runReady();
    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 0U);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, CancelledOfflineTimerIsGenerationGatedWhenFired)
{
    MockApplication app;
    auto script = loadRumbleFixtureScenario(QStringLiteral("offline-page"));
    script.exchanges.push_back(script.exchanges.front());
    FixtureLifecycle fixture(std::move(script));
    auto channel = fixture.channel(QStringLiteral("fixture-offline"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher,
                                QStringLiteral("fixture-offline"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Offline);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);

    connection.retry();
    // Fire the cancelled 30-second task adversarially before the new
    // never-inline zero-delay retry. Its captured generations are stale.
    fixture.scheduler.fireIgnoringCancellation(0);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 1U);
    fixture.manual.runReady();
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
    EXPECT_EQ(channel->state(), RumbleChannelState::Offline);

    connection.stop();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    // The replacement lifecycle installed a second offline timer at index 2.
    // Even a scheduler which violates cancellation suppression cannot revive
    // the stopped generation.
    fixture.scheduler.fireIgnoringCancellation(2);
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
}

TEST(RumbleLifecycle, ProviderShutdownCancelsActiveStreamAndLateTerminal)
{
    MockApplication app;
    constexpr std::int64_t THIRTY_DAYS_MS = 30LL * 24 * 60 * 60 * 1000;
    constexpr std::int64_t TERMINAL_MS = 365LL * 24 * 60 * 60 * 1000;
    FixtureLifecycle fixture(directStreamScript({streamExchange(
        "long-lived", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, TERMINAL_MS)}));
    auto channel = fixture.channel(QStringLiteral("shutdown-active"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    ASSERT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);
    fixture.manual.advanceBy(THIRTY_DAYS_MS);
    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);

    fixture.provider.shutdown();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
    fixture.manual.advanceBy(TERMINAL_MS - THIRTY_DAYS_MS);
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
}

TEST(RumbleLifecycle, OffOwnerCancellationClosesGateBeforeQtOwnedCleanup)
{
    MockApplication app;
    FixtureLifecycle fixture(directStreamScript({streamExchange(
        "off-owner-cancel", 200, {},
        {
            {.bytes = sse(
                 R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")},
            {.afterMs = 100,
             .bytes = sse(
                 R"({"type":"messages","data":{"messages":[{"id":"late","user_id":"u","channel_id":"c","text":"late","created_on":"2026-01-01T00:00:00Z"}]}})")},
        },
        RumbleFixtureTerminal::Disconnect, 10000)}));
    auto channel = fixture.channel(QStringLiteral("off-owner"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    ASSERT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);

    fixture.dispatcher->owner = false;
    channel->close();
    // Transport destruction is deliberately deferred to the owner, but the
    // callback gate is already closed. A late typed batch is inert.
    fixture.manual.advanceBy(100);
    EXPECT_EQ(channel->countMessages(), 0U);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);

    fixture.dispatcher->owner = true;
    fixture.dispatcher->runAll();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
    EXPECT_EQ(channel->countMessages(), 0U);
}

TEST(RumbleLifecycle, RejectedOffOwnerRestartClosesAndCancelsOnDisposal)
{
    MockApplication app;
    FixtureLifecycle fixture(directStreamScript({streamExchange(
        "rejected-restart", 200, {},
        {
            {.bytes = sse(
                 R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")},
            {.afterMs = 100,
             .bytes = sse(
                 R"({"type":"messages","data":{"messages":[{"id":"late","user_id":"u","channel_id":"c","text":"late","created_on":"2026-01-01T00:00:00Z"}]}})")},
        },
        RumbleFixtureTerminal::Disconnect, 10000)}));
    auto channel = fixture.channel(QStringLiteral("rejected-restart"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);

    fixture.dispatcher->owner = false;
    fixture.dispatcher->accept = false;
    connection.restart(QStringLiteral("1002"));
    fixture.manual.advanceBy(100);
    EXPECT_EQ(channel->countMessages(), 0U);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);

    fixture.dispatcher->owner = true;
    fixture.dispatcher->runAll();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
}

TEST(RumbleLifecycle, ContextRestartCancelsFirstAndResolvesNewGeneration)
{
    MockApplication app;
    auto first = streamExchange(
        "first-context", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000, "1001");
    auto second = streamExchange(
        "second-context", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000, "1002");
    FixtureLifecycle fixture(
        directStreamScript({std::move(first), std::move(second)}));
    auto channel = fixture.channel(QStringLiteral("context-change"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
    ASSERT_EQ(channel->getCurrentStreamID(), QStringLiteral("1001"));
    ASSERT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);

    connection.restart(QStringLiteral("1002"));
    // Cancellation happens before the never-inline replacement generation.
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
    fixture.manual.runReady();
    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    EXPECT_EQ(channel->getCurrentStreamID(), QStringLiteral("1002"));
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 1U);
    EXPECT_EQ(fixture.fixtureTransport.remainingExchangeCount(), 0U);
}

TEST(RumbleLifecycle, SchedulerFailureDuringManualRetryFailsClosed)
{
    MockApplication app;
    FixtureLifecycle fixture(directStreamScript({streamExchange(
        "connected", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10000)}));
    auto channel = fixture.channel(QStringLiteral("scheduler-failure"));
    ASSERT_TRUE(channel);
    RumbleConnection connection(channel, fixture.api, fixture.scheduler,
                                fixture.dispatcher, QStringLiteral("1001"));
    ASSERT_TRUE(connection.start());
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);

    fixture.scheduler.rejectSchedules = true;
    connection.retry();
    EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
    fixture.dispatcher->runAll();
    EXPECT_EQ(channel->state(), RumbleChannelState::Failed);
    EXPECT_EQ(channel->lifecycleMetadata().retryCause,
              RumbleRetryCause::SchedulerUnavailable);
    EXPECT_EQ(fixture.fixtureTransport.activeRequestCount(), 0U);
}

TEST(RumbleLifecycle, InvalidOwnerSeamsFailClosedWithoutDereference)
{
    MockApplication app;
    ManualScheduler manual;
    FixtureLifecycleScheduler scheduler(manual);
    RumbleFixtureTransport transport(manual,
                                     {.name = "empty", .exchanges = {}});
    RumbleFixtureApiTransport adapter(transport);
    RumbleApi api(adapter, [&](std::function<void()> task) {
        std::ignore = manual.scheduleAfter(0, std::move(task));
    });

    RumbleConnection nulls(nullptr, api, scheduler, nullptr,
                           QStringLiteral("fixture-channel"));
    EXPECT_FALSE(nulls.start());
    nulls.retry();
    nulls.stop();
}

TEST(RumbleLifecycle, QtSchedulerIsNeverInlineAndAcceptsLongDelays)
{
    QObject owner;
    int randomCalls = 0;
    QtRumbleScheduler scheduler(&owner, [&] {
        ++randomCalls;
        return std::numeric_limits<std::uint64_t>::max();
    });

    bool ran = false;
    auto immediate = scheduler.scheduleAfter(0, [&] {
        ran = true;
    });
    ASSERT_TRUE(immediate);
    EXPECT_FALSE(ran);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_TRUE(ran);
    EXPECT_FALSE(immediate->active());

    auto longDelay = scheduler.scheduleAfter(
        static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1, [] {
            FAIL() << "long delay ran without fake-time advancement";
        });
    ASSERT_TRUE(longDelay);
    EXPECT_TRUE(longDelay->active());
    longDelay->cancel();
    EXPECT_FALSE(longDelay->active());

    const auto random = scheduler.randomBelow(100);
    EXPECT_LT(random, 100U);
    EXPECT_EQ(randomCalls, 8);
}
