// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/RumbleFixtureApiTransport.hpp"
#include "lib/RumbleFixtureLoader.hpp"
#include "lib/RumbleFixtureTransport.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Logging.hpp"
#include "providers/rumble/RumbleApi.hpp"
#include "providers/rumble/RumbleApplicationController.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/rumble/RumbleModeration.hpp"
#include "providers/rumble/RumbleScheduler.hpp"
#include "providers/rumble/RumbleSession.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"

#include <QDateTime>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace chatterino;
using namespace chatterino::rumble;
using namespace chatterino::test;

namespace {

class ReleaseApplication final : public mock::BaseApplication
{
public:
    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    mock::EmptyLogging logging;
};

class ReleaseDispatcher final : public RumbleDispatcher
{
public:
    bool isOwnerThread() const noexcept override
    {
        return this->owner;
    }

    bool dispatch(Task task) override
    {
        if (!this->accept)
        {
            return false;
        }
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

    [[nodiscard]] std::size_t pending() const noexcept
    {
        return this->tasks.size();
    }

    bool owner = true;
    bool accept = true;
    std::vector<Task> tasks;
};

struct ReleaseScheduledState {
    ManualScheduler *scheduler = nullptr;
    ManualScheduler::TaskId id = 0;
    std::atomic_bool active{true};
    std::function<void()> callback;
};

class ReleaseScheduledTask final : public ScheduledTask
{
public:
    explicit ReleaseScheduledTask(std::shared_ptr<ReleaseScheduledState> state)
        : state_(std::move(state))
    {
    }

    ~ReleaseScheduledTask() override
    {
        this->cancel();
    }

    void cancel() noexcept override
    {
        if (this->state_ &&
            this->state_->active.exchange(false, std::memory_order_acq_rel))
        {
            this->state_->scheduler->cancel(this->state_->id);
        }
    }

    bool active() const noexcept override
    {
        return this->state_ &&
               this->state_->active.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<ReleaseScheduledState> state_;
};

class ReleaseScheduler final : public RumbleScheduler
{
public:
    explicit ReleaseScheduler(ManualScheduler &scheduler)
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
        auto state = std::make_shared<ReleaseScheduledState>();
        state->scheduler = &this->scheduler_;
        state->callback = std::move(callback);
        const std::weak_ptr weak = state;
        state->id = this->scheduler_.scheduleAfter(delayMs, [weak] {
            const auto locked = weak.lock();
            if (!locked ||
                !locked->active.exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            locked->callback();
        });
        this->states_.push_back(state);
        return std::make_unique<ReleaseScheduledTask>(std::move(state));
    }

    std::uint64_t randomBelow(std::uint64_t upper) override
    {
        this->randomUpperBounds.push_back(upper);
        return upper == 0
                   ? 0
                   : std::min<std::uint64_t>(this->randomValue, upper - 1);
    }

    [[nodiscard]] std::size_t activeTaskCount() const noexcept
    {
        return std::ranges::count_if(this->states_, [](const auto &state) {
            return state->active.load(std::memory_order_acquire);
        });
    }

    std::uint64_t randomValue = 500;
    std::vector<std::uint64_t> randomUpperBounds;

private:
    ManualScheduler &scheduler_;
    std::vector<std::shared_ptr<ReleaseScheduledState>> states_;
};

class ReleaseAuthHandle final : public AuthHandle
{
public:
    explicit ReleaseAuthHandle(bool &cancelled)
        : cancelled_(cancelled)
    {
    }

    void cancel() noexcept override
    {
        this->cancelled_ = true;
    }

private:
    bool &cancelled_;
};

class ReleaseAuthTransport final : public AuthTransport
{
public:
    std::unique_ptr<AuthHandle> start(AuthOperation operation, QString stream,
                                      QString text, QByteArray bearer,
                                      QByteArray requestId,
                                      AuthCallbacks callbacks) override
    {
        ++this->starts;
        this->lastOperation = operation;
        this->lastStream = std::move(stream);
        this->lastText = std::move(text);
        this->sawSyntheticBearer =
            bearer == QByteArrayLiteral("SYNTHETIC_RELEASE_SESSION");
        bearer.fill('\0');
        bearer.clear();
        this->lastRequestIdSize = requestId.size();
        requestId.fill('\0');
        requestId.clear();
        this->pending = std::move(callbacks);
        this->cancelled = false;
        return std::make_unique<ReleaseAuthHandle>(this->cancelled);
    }

    void complete(int status, QByteArray body,
                  std::optional<QByteArray> retryAfter = std::nullopt)
    {
        auto callback = std::move(this->pending.complete);
        this->pending = {};
        ASSERT_TRUE(callback);
        callback({status, QByteArrayLiteral("application/json"),
                  std::move(body), std::move(retryAfter)});
    }

    int starts = 0;
    bool cancelled = false;
    bool sawSyntheticBearer = false;
    AuthOperation lastOperation = AuthOperation::Probe;
    QString lastStream;
    QString lastText;
    qsizetype lastRequestIdSize = 0;
    AuthCallbacks pending;
};

class ReleaseRouteChannel final : public Channel
{
public:
    ReleaseRouteChannel(QString name, MultiChannel::Platform platform)
        : Channel(std::move(name), channelType(platform))
    {
        switch (platform)
        {
            case MultiChannel::Platform::Twitch:
                this->context.platform = QStringLiteral("twitch");
                break;
            case MultiChannel::Platform::Kick:
                this->context.platform = QStringLiteral("kick");
                break;
            case MultiChannel::Platform::Rumble:
                this->context.platform = QStringLiteral("rumble");
                break;
        }
        this->context.channelID = this->getName();
        this->context.accountID =
            this->context.platform + QStringLiteral("-release-account");
        this->context.writable = true;
        this->context.authenticated = true;
    }

    static Channel::Type channelType(MultiChannel::Platform platform)
    {
        switch (platform)
        {
            case MultiChannel::Platform::Twitch:
                return Channel::Type::Twitch;
            case MultiChannel::Platform::Kick:
                return Channel::Type::Kick;
            case MultiChannel::Platform::Rumble:
                return Channel::Type::Rumble;
        }
        return Channel::Type::None;
    }

    bool canSendMessage() const override
    {
        return this->context.writable && this->context.authenticated;
    }

    bool isWritable() const override
    {
        return this->context.writable;
    }

    MessageSendContext messageSendContext() const override
    {
        return this->context;
    }

    void sendMessage(const QString &message) override
    {
        this->sent.push_back(message);
    }

    void sendMessageAsync(QString message, SendCallback callback) override
    {
        this->sent.push_back(std::move(message));
        if (callback)
        {
            callback({SendOutcome::Confirmed, {}});
        }
    }

    MessageSendContext context;
    std::vector<QString> sent;
};

struct ReleaseFixture {
    explicit ReleaseFixture(RumbleFixtureScript script)
        : transport(this->manual, std::move(script))
        , apiTransport(this->transport)
        , scheduler(this->manual)
        , api(this->apiTransport,
              [this](std::function<void()> task) {
                  std::ignore = this->manual.scheduleAfter(0, std::move(task));
              })
        , controller(std::make_unique<RumbleApplicationController>(
              this->api, this->scheduler, this->dispatcher))
    {
    }

    ManualScheduler manual;
    std::shared_ptr<ReleaseDispatcher> dispatcher =
        std::make_shared<ReleaseDispatcher>();
    RumbleFixtureTransport transport;
    RumbleFixtureApiTransport apiTransport;
    ReleaseScheduler scheduler;
    RumbleApi api;
    std::unique_ptr<RumbleApplicationController> controller;
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
    std::int64_t terminalAfterMs = 0)
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
                .target = "/chat/api/chat/1001/stream",
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

RumbleFixtureScript resolvedScript(
    std::vector<RumbleFixtureExchange> streamExchanges)
{
    auto script = loadRumbleFixtureScenario(QStringLiteral("live-session"));
    if (script.exchanges.size() != 3)
    {
        throw std::logic_error(
            "live-session must contain page, embed, and stream exchanges");
    }
    script.name = "release-gate-resolved-stream";
    script.exchanges.resize(2);
    script.exchanges.insert(script.exchanges.end(),
                            std::make_move_iterator(streamExchanges.begin()),
                            std::make_move_iterator(streamExchanges.end()));
    return script;
}

RumbleFixtureScript connectedScript()
{
    return resolvedScript({streamExchange(
        "release-connected", 200, {},
        {{.bytes = sse(
              R"({"type":"init","data":{"users":{},"channels":{},"config":{"message_length_max":200},"messages":[]}})")}},
        RumbleFixtureTerminal::Disconnect, 10'000)});
}

MultiChannel::Spec releaseRumbleSpec(QString locator)
{
    return {
        .platform = MultiChannel::Platform::Rumble,
        .name = locator,
        .layoutIdentity =
            ChannelLayoutIdentity{
                .platform = QStringLiteral("rumble"),
                .locator = std::move(locator),
            },
    };
}

MultiChannel::Spec releaseNamedSpec(MultiChannel::Platform platform,
                                    QString name)
{
    return {
        .platform = platform,
        .name = std::move(name),
    };
}

std::shared_ptr<MultiChannel> releaseMulti(
    std::span<const MultiChannel::Spec> specs, std::vector<ChannelPtr> channels)
{
    auto next = std::make_shared<std::size_t>(0);
    return std::make_shared<MultiChannel>(
        specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [channels = std::move(channels),
         next](const MultiChannel::Spec &) mutable -> ChannelPtr {
            if (*next >= channels.size())
            {
                return Channel::getEmpty();
            }
            return channels[(*next)++];
        });
}

std::shared_ptr<RumbleChannel> rumbleRuntime(const IndirectChannel &view)
{
    return std::dynamic_pointer_cast<RumbleChannel>(view.get());
}

}  // namespace

TEST(RumbleReleaseGate, PublicLifecycleReconnectsWithoutDuplicates)
{
    ReleaseApplication app;
    auto reconnect = loadRumbleFixtureScenario(QStringLiteral("reconnect"));
    ASSERT_EQ(reconnect.exchanges.size(), 2U);
    reconnect.exchanges[1].chunks = {{
        .bytes =
            sse(R"({"type":"init","data":{"users":{"u_fixture":{"username":"fixture-user","badges":["moderator"],"roles":["moderator"]}},"channels":{"c_fixture":{"title":"Fixture Channel"}},"config":{"badges":{"moderator":{"label":{"en":"Moderator"},"icons":{"48":"/i/badges/moderator_48.png"}}},"message_length_max":200},"messages":[{"id":"m3","user_id":"u_fixture","channel_id":"c_fixture","text":"fixture third","created_on":"2026-01-01T00:00:03Z"},{"id":"m1","user_id":"u_fixture","channel_id":"c_fixture","text":"fixture first window","created_on":"2026-01-01T00:00:00Z"},{"id":"m2","user_id":"u_fixture","channel_id":"c_fixture","text":"fixture second","created_on":"2026-01-01T00:00:02Z"}]}})") +
            sse(R"({"type":"messages","data":{"messages":[{"id":"m3","user_id":"u_fixture","channel_id":"c_fixture","text":"fixture duplicate","created_on":"2026-01-01T00:00:03Z"},{"id":"m4","user_id":"u_fixture","channel_id":"c_fixture","text":"fixture https://example.com/release","created_on":"2026-01-01T00:00:04Z"}]}})") +
            sse(R"({"type":"pin_message","data":{"message":{"id":"pin1","user_id":"u_fixture","channel_id":"c_fixture","text":"fixture pinned","created_on":"2026-01-01T00:00:05Z"}}})") +
            sse(R"({"type":"delete_messages","data":{"message_ids":["m1"]}})") +
            "data: {not-json}\n\n",
    }};
    reconnect.exchanges[1].terminal = RumbleFixtureTerminal::Disconnect;
    reconnect.exchanges[1].terminalAfterMs = 10'000;

    std::weak_ptr<RumbleChannel> releasedChannel;
    {
        int callbackCount = 0;
        ReleaseFixture fixture(resolvedScript(std::move(reconnect.exchanges)));
        auto view = fixture.controller->restore(QStringLiteral(
            "https://rumble.com/c/fixture-channel?discard=private"));
        ASSERT_TRUE(view.layoutIdentity());
        EXPECT_EQ(view.layoutIdentity()->locator,
                  QStringLiteral("https://rumble.com/c/fixture-channel"));
        auto channel = rumbleRuntime(view);
        ASSERT_TRUE(channel);
        releasedChannel = channel;

        std::ignore = channel->stateChanged.connect(
            [&](RumbleChannelState, RumbleChannelState) {
                ++callbackCount;
            });
        std::ignore = channel->messageAppended.connect(
            [&](const MessagePtr &, std::optional<MessageFlags>) {
                ++callbackCount;
            });
        std::ignore = channel->filledInMessages.connect(
            [&](const std::vector<MessagePtr> &) {
                ++callbackCount;
            });

        fixture.manual.runReady();
        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
        ASSERT_EQ(channel->getMessageSnapshot().size(), 1U);
        EXPECT_EQ(channel->getMessageSnapshot()[0]->id, QStringLiteral("m1"));

        fixture.manual.advanceBy(5);
        ASSERT_EQ(channel->state(), RumbleChannelState::Backoff);
        ASSERT_EQ(fixture.scheduler.randomUpperBounds,
                  std::vector<std::uint64_t>{1001});
        fixture.manual.advanceBy(500);

        ASSERT_EQ(channel->state(), RumbleChannelState::Connected);
        const auto messages = channel->getMessageSnapshot();
        ASSERT_EQ(messages.size(), 4U);
        EXPECT_EQ(messages[0]->id, QStringLiteral("m1"));
        EXPECT_EQ(messages[1]->id, QStringLiteral("m2"));
        EXPECT_EQ(messages[2]->id, QStringLiteral("m3"));
        EXPECT_EQ(messages[3]->id, QStringLiteral("m4"));
        EXPECT_EQ(std::ranges::count_if(messages,
                                        [](const auto &message) {
                                            return message->id ==
                                                   QStringLiteral("m3");
                                        }),
                  1);
        EXPECT_EQ(messages[3]->messageText,
                  QStringLiteral("fixture https://example.com/release"));
        EXPECT_EQ(messages[3]->platform, MessagePlatform::Rumble);
        EXPECT_TRUE(messages[0]->flags.has(MessageFlag::Disabled));
        const auto linkElement = std::ranges::find_if(
            messages[3]->elements, [](const auto &element) {
                return dynamic_cast<const LinkElement *>(element.get()) !=
                       nullptr;
            });
        ASSERT_NE(linkElement, messages[3]->elements.end());
        const auto *link =
            dynamic_cast<const LinkElement *>(linkElement->get());
        ASSERT_NE(link, nullptr);
        EXPECT_EQ(link->getLink().type, Link::Url);
        EXPECT_EQ(link->getLink().value,
                  QStringLiteral("https://example.com/release"));
        ASSERT_TRUE(messages[1]->rumble);
        EXPECT_EQ(messages[1]->rumble->badgeIDs,
                  QStringList{QStringLiteral("moderator")});
        EXPECT_EQ(messages[1]->rumble->roleIDs,
                  QStringList{QStringLiteral("moderator")});
        EXPECT_EQ(messages[1]->serverReceivedTime,
                  QDateTime::fromString(QStringLiteral("2026-01-01T00:00:02Z"),
                                        Qt::ISODate)
                      .toUTC());
        const auto badgeElement = std::ranges::find_if(
            messages[1]->elements, [](const auto &element) {
                return element->getFlags() ==
                       MessageElementFlag::BadgeChannelAuthority;
            });
        ASSERT_NE(badgeElement, messages[1]->elements.end());
        const auto *badge =
            dynamic_cast<const BadgeElement *>(badgeElement->get());
        ASSERT_NE(badge, nullptr);
        EXPECT_EQ(badge->getTooltip(), QStringLiteral("Moderator"));
        EXPECT_TRUE(std::ranges::none_of(
            messages[1]->elements, [](const auto &element) {
                return dynamic_cast<const EmoteElement *>(element.get()) !=
                       nullptr;
            }));
        ASSERT_TRUE(channel->pinnedMessage());
        EXPECT_EQ(channel->pinnedMessage()->publication.id().value(),
                  QStringLiteral("pin1"));
        EXPECT_EQ(fixture.transport.remainingExchangeCount(), 0U);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 1U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
        EXPECT_GT(callbackCount, 0);

        const auto callbacksBeforeShutdown = callbackCount;
        fixture.controller->beginShutdown();
        fixture.controller->shutdown();
        fixture.dispatcher->runAll();
        fixture.manual.runUntilIdle();
        EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
        EXPECT_GE(callbackCount, callbacksBeforeShutdown);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
        EXPECT_EQ(fixture.manual.pendingTaskCount(), 0U);
        EXPECT_EQ(fixture.dispatcher->pending(), 0U);
        const auto callbacksAfterShutdown = callbackCount;
        fixture.dispatcher->runAll();
        fixture.manual.runUntilIdle();
        EXPECT_EQ(callbackCount, callbacksAfterShutdown);
    }
    EXPECT_TRUE(releasedChannel.expired());
}

TEST(RumbleReleaseGate, OfflineAndInvalidLocatorsStaySafeAndLocal)
{
    ReleaseApplication app;
    ReleaseFixture fixture(
        loadRumbleFixtureScenario(QStringLiteral("offline-page")));

    const auto exchangesBefore = fixture.transport.remainingExchangeCount();
    auto unsafe = fixture.controller->restore(QStringLiteral(
        "https://example.test/c/private?secret=must-not-survive"));
    EXPECT_EQ(unsafe.getType(), Channel::Type::Rumble);
    ASSERT_TRUE(unsafe.layoutIdentity());
    EXPECT_TRUE(unsafe.layoutIdentity()->locator.isEmpty());
    EXPECT_FALSE(
        unsafe.get()->getName().contains(QStringLiteral("must-not-survive")));
    EXPECT_EQ(fixture.transport.remainingExchangeCount(), exchangesBefore);
    EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);

    auto view = fixture.controller->restore(
        QStringLiteral("https://rumble.com/c/fixture-offline"));
    auto channel = rumbleRuntime(view);
    ASSERT_TRUE(channel);
    fixture.manual.runReady();

    ASSERT_EQ(channel->state(), RumbleChannelState::Offline);
    ASSERT_TRUE(channel->lifecycleMetadata().deadlineAtMs);
    EXPECT_EQ(*channel->lifecycleMetadata().deadlineAtMs, 30'000);
    // Merged #20 keeps anonymous/offline channels non-writable. Only a
    // connected stream with an explicitly imported, validated session can
    // enable normal-text sending.
    EXPECT_FALSE(channel->canSendMessage());
    EXPECT_FALSE(channel->isWritable());
    const auto sendContext = channel->messageSendContext();
    EXPECT_EQ(sendContext.platform, QStringLiteral("rumble"));
    EXPECT_FALSE(sendContext.writable);
    EXPECT_FALSE(sendContext.authenticated);
    EXPECT_TRUE(sendContext.emoteCapabilitiesComplete);
    EXPECT_EQ(fixture.transport.remainingExchangeCount(), 0U);
    EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
    EXPECT_EQ(fixture.scheduler.activeTaskCount(), 1U);

    fixture.controller->beginShutdown();
    fixture.controller->shutdown();
    fixture.dispatcher->runAll();
    fixture.manual.runUntilIdle();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
    EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
    EXPECT_EQ(fixture.manual.pendingTaskCount(), 0U);
    EXPECT_EQ(fixture.dispatcher->pending(), 0U);
}

TEST(RumbleReleaseGate, FailuresAndOwnerDestructionLeaveNoResidue)
{
    ReleaseApplication app;

    {
        auto forbidden =
            loadRumbleFixtureScenario(QStringLiteral("live-session"));
        ASSERT_EQ(forbidden.exchanges.size(), 3U);
        forbidden.name = "release-gate-rejected-resolution";
        forbidden.exchanges.resize(1);
        forbidden.exchanges[0].response.status = 400;
        forbidden.exchanges[0].chunks.clear();
        forbidden.exchanges[0].terminalAfterMs = 0;
        ReleaseFixture fixture(std::move(forbidden));

        auto view = fixture.controller->restore(
            QStringLiteral("https://rumble.com/c/fixture-channel"));
        auto channel = rumbleRuntime(view);
        ASSERT_TRUE(channel);
        fixture.manual.runReady();

        ASSERT_EQ(channel->state(), RumbleChannelState::Failed);
        ASSERT_TRUE(channel->failure());
        EXPECT_EQ(channel->failure()->code(), RumbleFailureCode::RejectedHttp);
        const auto safeText = channel->failure()->operatorSafeText();
        ASSERT_TRUE(safeText);
        EXPECT_FALSE(safeText->contains(QStringLiteral("fixture-channel")));
        EXPECT_EQ(channel->lifecycleMetadata().retryCause,
                  RumbleRetryCause::HttpFailure);
        EXPECT_FALSE(channel->lifecycleMetadata().deadlineAtMs);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);

        fixture.controller->shutdown();
        fixture.dispatcher->runAll();
        fixture.manual.runUntilIdle();
        EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
        EXPECT_EQ(fixture.manual.pendingTaskCount(), 0U);
        EXPECT_EQ(fixture.dispatcher->pending(), 0U);
    }

    {
        auto limited = streamExchange(
            "release-rate-limit", 429,
            {{"Content-Type", "application/json"}, {"Retry-After", "3"}});
        auto connected = streamExchange(
            "release-recovered", 200, {},
            {{.bytes = sse(
                  R"({"type":"init","data":{"users":{},"channels":{},"config":{},"messages":[]}})")}},
            RumbleFixtureTerminal::Disconnect, 10'000);
        ReleaseFixture fixture(
            resolvedScript({std::move(limited), std::move(connected)}));

        auto view = fixture.controller->restore(
            QStringLiteral("https://rumble.com/c/fixture-channel"));
        auto channel = rumbleRuntime(view);
        ASSERT_TRUE(channel);
        fixture.manual.runReady();

        const auto limitedState = channel->lifecycleSnapshot();
        ASSERT_EQ(limitedState.state, RumbleChannelState::Backoff);
        EXPECT_TRUE(limitedState.metadata.rateLimited);
        EXPECT_EQ(limitedState.metadata.retryCause,
                  RumbleRetryCause::RateLimited);
        ASSERT_TRUE(limitedState.metadata.deadlineAtMs);
        EXPECT_EQ(*limitedState.metadata.deadlineAtMs, 3'000);
        EXPECT_TRUE(fixture.scheduler.randomUpperBounds.empty());
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 1U);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);

        fixture.manual.advanceBy(2'999);
        EXPECT_EQ(fixture.transport.remainingExchangeCount(), 1U);
        EXPECT_EQ(channel->state(), RumbleChannelState::Backoff);
        fixture.manual.advanceBy(1);
        EXPECT_EQ(channel->state(), RumbleChannelState::Connected);
        EXPECT_FALSE(channel->lifecycleMetadata().rateLimited);
        EXPECT_EQ(channel->lifecycleMetadata().consecutiveFailures, 0U);
        EXPECT_EQ(fixture.transport.remainingExchangeCount(), 0U);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 1U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);

        fixture.controller->beginShutdown();
        fixture.controller->shutdown();
        fixture.dispatcher->runAll();
        fixture.manual.runUntilIdle();
        EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
        EXPECT_EQ(fixture.manual.pendingTaskCount(), 0U);
        EXPECT_EQ(fixture.dispatcher->pending(), 0U);
    }

    {
        auto delayed =
            loadRumbleFixtureScenario(QStringLiteral("live-session"));
        ASSERT_EQ(delayed.exchanges.size(), 3U);
        delayed.name = "release-gate-owner-destruction";
        delayed.exchanges[0].headAfterMs = 500;
        int callbackCount = 0;
        ReleaseFixture fixture(std::move(delayed));

        auto view = fixture.controller->restore(
            QStringLiteral("https://rumble.com/c/fixture-channel"));
        auto channel = rumbleRuntime(view);
        ASSERT_TRUE(channel);
        std::weak_ptr<RumbleChannel> releasedChannel = channel;

        auto callbackSentinel = std::make_shared<int>(1);
        std::weak_ptr<int> releasedCallback = callbackSentinel;
        auto request = fixture.controller->resolve(
            QStringLiteral("https://rumble.com/c/fixture-channel"),
            [callbackSentinel, &callbackCount](auto) {
                (void)callbackSentinel;
                ++callbackCount;
            });
        callbackSentinel.reset();

        EXPECT_TRUE(request.active());
        EXPECT_EQ(fixture.transport.activeRequestCount(), 1U);
        fixture.controller.reset();
        fixture.dispatcher->runAll();
        fixture.manual.runUntilIdle();

        EXPECT_FALSE(request.active());
        EXPECT_EQ(callbackCount, 0);
        EXPECT_TRUE(releasedCallback.expired());
        EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
        EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
        EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
        EXPECT_EQ(fixture.manual.pendingTaskCount(), 0U);
        EXPECT_EQ(fixture.dispatcher->pending(), 0U);

        view.reset(nullptr);
        channel.reset();
        EXPECT_TRUE(releasedChannel.expired());
    }
}

TEST(RumbleReleaseGate,
     ImportedSessionSendsNormalTextAtMostOnceAndClearsLocally)
{
    ReleaseApplication app;
    ReleaseFixture fixture(connectedScript());
    auto view = fixture.controller->restore(
        QStringLiteral("https://rumble.com/c/fixture-channel"));
    auto channel = rumbleRuntime(view);
    ASSERT_TRUE(channel);
    fixture.manual.runReady();
    ASSERT_EQ(channel->state(), RumbleChannelState::Connected);

    ReleaseAuthTransport auth;
    auto session = std::make_shared<SessionController>(auth);
    channel->setSessionController(session);
    ASSERT_TRUE(
        session->importSession(QByteArrayLiteral("SYNTHETIC_RELEASE_SESSION")));
    bool validated = false;
    session->validate([&](bool accepted, QString message) {
        EXPECT_TRUE(accepted);
        EXPECT_EQ(message, QStringLiteral("Rumble sign-in confirmed."));
        EXPECT_FALSE(
            message.contains(QStringLiteral("SYNTHETIC_RELEASE_SESSION")));
        validated = true;
    });
    ASSERT_EQ(auth.starts, 1);
    EXPECT_EQ(auth.lastOperation, AuthOperation::Probe);
    EXPECT_TRUE(auth.sawSyntheticBearer);
    auth.complete(200, QByteArrayLiteral(R"({"user":{"id":"release-user"}})"));
    ASSERT_TRUE(validated);
    ASSERT_EQ(session->state(), SessionState::Valid);
    EXPECT_TRUE(channel->canSendMessage());
    EXPECT_TRUE(channel->isWritable());
    const auto context = channel->messageSendContext();
    EXPECT_EQ(context.platform, QStringLiteral("rumble"));
    EXPECT_TRUE(context.writable);
    EXPECT_TRUE(context.authenticated);
    EXPECT_EQ(context.maxMessageLength, 200);
    EXPECT_TRUE(context.emoteCapabilitiesComplete);
    EXPECT_TRUE(context.providerConstraints.empty());
    EXPECT_TRUE(evaluateMessageDraft(
                    MessageDraft::fromPlainText(QStringLiteral("release text")),
                    context)
                    .sendable);

    int callbacks = 0;
    channel->sendMessageAsync(
        QStringLiteral("release text"), [&](Channel::SendResult result) {
            EXPECT_EQ(result.outcome, Channel::SendOutcome::Confirmed);
            EXPECT_TRUE(result.userMessage.isEmpty());
            ++callbacks;
        });
    ASSERT_EQ(auth.starts, 2);
    EXPECT_EQ(auth.lastOperation, AuthOperation::Send);
    EXPECT_EQ(auth.lastStream, QStringLiteral("1001"));
    EXPECT_EQ(auth.lastText, QStringLiteral("release text"));
    EXPECT_EQ(auth.lastRequestIdSize, 43);
    EXPECT_EQ(callbacks, 0);
    auth.complete(200,
                  QByteArrayLiteral(R"({"data":{"id":"release-message"}})"));
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(auth.starts, 2);

    session->clear();
    EXPECT_EQ(session->state(), SessionState::Empty);
    EXPECT_TRUE(session->accountId().isEmpty());
    EXPECT_FALSE(channel->canSendMessage());
    std::weak_ptr<SessionController> releasedSession = session;
    session.reset();
    EXPECT_TRUE(releasedSession.expired());

    fixture.controller->beginShutdown();
    fixture.controller->shutdown();
    fixture.dispatcher->runAll();
    fixture.manual.runUntilIdle();
    EXPECT_EQ(channel->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
    EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
}

TEST(RumbleReleaseGate, ModerationStateIsInboundOnlyAndOutboundAlwaysExplicit)
{
    auto account =
        ModerationIdentity::fromProvider(QStringLiteral("release-account"));
    auto channel =
        ModerationIdentity::fromProvider(QStringLiteral("release-channel"));
    auto message =
        ModerationIdentity::fromProvider(QStringLiteral("release-message"));
    auto user =
        ModerationIdentity::fromProvider(QStringLiteral("release-user"));
    ASSERT_TRUE(account);
    ASSERT_TRUE(channel);
    ASSERT_TRUE(message);
    ASSERT_TRUE(user);

    const ModerationScope scope{*account, *channel};
    ModerationState state(scope);
    EXPECT_EQ(
        state.apply(
            {0, ModerationRoleSnapshot{scope, {ModerationRole::Moderator}}}),
        ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({1, ModerationMessageDeleted{scope, {*message}}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({2, ModerationPinChanged{scope, *message}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({3, ModerationMuteChanged{scope, *user, true}}),
              ModerationApplyResult::Applied);
    EXPECT_EQ(state.apply({4, ModerationMessageDeleted{scope, {*message}}}),
              ModerationApplyResult::Duplicate);

    const auto snapshot = moderationSnapshot(state);
    EXPECT_TRUE(
        snapshot.deletedMessageIds.contains(QStringLiteral("release-message")));
    ASSERT_TRUE(snapshot.pinnedMessageId);
    EXPECT_EQ(*snapshot.pinnedMessageId, QStringLiteral("release-message"));
    EXPECT_TRUE(snapshot.mutedUserIds.contains(QStringLiteral("release-user")));
    EXPECT_EQ(snapshot.capabilities.get(ModerationCapability::ObserveDeletes),
              ModerationAvailability::Available);
    for (const auto capability : {
             ModerationCapability::DeleteMessage,
             ModerationCapability::PinMessage,
             ModerationCapability::UnpinMessage,
             ModerationCapability::MuteUser,
             ModerationCapability::UnmuteUser,
             ModerationCapability::BanUser,
             ModerationCapability::UnbanUser,
         })
    {
        EXPECT_EQ(snapshot.capabilities.get(capability),
                  ModerationAvailability::Unsupported);
        const auto mutation = requestModerationMutation(capability);
        ASSERT_FALSE(mutation);
        EXPECT_EQ(mutation.error(), ModerationMutationError::Unsupported);
    }
}

TEST(RumbleReleaseGate,
     MultiChannelKeepsCanonicalRowsAndDispatchesOneCurrentDestination)
{
    ReleaseApplication app;
    ReleaseFixture fixture(connectedScript());
    auto view = fixture.controller->restore(
        QStringLiteral("https://rumble.com/c/fixture-channel"));
    auto rumble = rumbleRuntime(view);
    ASSERT_TRUE(rumble);
    fixture.manual.runReady();
    ASSERT_EQ(rumble->state(), RumbleChannelState::Connected);

    ReleaseAuthTransport auth;
    auto session = std::make_shared<SessionController>(auth);
    rumble->setSessionController(session);
    ASSERT_TRUE(
        session->importSession(QByteArrayLiteral("SYNTHETIC_RELEASE_SESSION")));
    session->validate({});
    auth.complete(200, QByteArrayLiteral(R"({"user":{"id":"release-user"}})"));
    ASSERT_TRUE(rumble->canSendMessage());

    auto twitch = std::make_shared<ReleaseRouteChannel>(
        QStringLiteral("release-twitch"), MultiChannel::Platform::Twitch);
    auto kick = std::make_shared<ReleaseRouteChannel>(
        QStringLiteral("release-kick"), MultiChannel::Platform::Kick);
    const std::array specs{
        releaseRumbleSpec(
            QStringLiteral("https://rumble.com/c/fixture-channel")),
        releaseRumbleSpec(QStringLiteral("https://rumble.com/embed/vfixture")),
        releaseNamedSpec(MultiChannel::Platform::Twitch,
                         QStringLiteral("release-twitch")),
        releaseNamedSpec(MultiChannel::Platform::Kick,
                         QStringLiteral("release-kick")),
    };
    auto multi = releaseMulti(specs, {rumble, rumble, twitch, kick});
    ASSERT_EQ(multi->channels().size(), 4U);
    EXPECT_TRUE(multi->channels()[0].primaryRuntimeChannel);
    EXPECT_FALSE(multi->channels()[1].primaryRuntimeChannel);
    ASSERT_TRUE(multi->channels()[0].spec().layoutIdentity);
    ASSERT_TRUE(multi->channels()[1].spec().layoutIdentity);
    EXPECT_EQ(multi->channels()[0].spec().layoutIdentity->locator,
              QStringLiteral("https://rumble.com/c/fixture-channel"));
    EXPECT_EQ(multi->channels()[1].spec().layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vfixture"));
    multi->setActiveChannelIndex(1);
    ASSERT_NE(multi->activeChannel(), nullptr);
    EXPECT_EQ(multi->activeChannelIndex(), 1U);
    EXPECT_EQ(multi->activeChannel()->channel.get(), rumble.get());

    int callbacks = 0;
    std::optional<MultiChannel::DraftDispatchResult> completedRoute;
    auto result = multi->sendMessageDraftAsync(
        MessageDraft::fromPlainText(QStringLiteral("one destination")),
        QStringLiteral("one destination"), multi->activeChannelIndex(),
        MultiChannelRoutePolicy::CompatibleFallback,
        [&](MultiChannel::DraftDispatchResult completed,
            Channel::SendResult send) {
            EXPECT_EQ(completed.destinationIndex, 1U);
            EXPECT_EQ(completed.destination.get(), rumble.get());
            EXPECT_EQ(completed.destinationPlatform, QStringLiteral("rumble"));
            EXPECT_EQ(completed.evaluations.size(), 4U);
            EXPECT_FALSE(completed.usedFallback);
            EXPECT_EQ(send.outcome, Channel::SendOutcome::Confirmed);
            completedRoute = std::move(completed);
            ++callbacks;
        });
    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1U);
    EXPECT_EQ(result.destination.get(), rumble.get());
    EXPECT_EQ(result.destinationPlatform, QStringLiteral("rumble"));
    EXPECT_EQ(result.evaluations.size(), 4U);
    EXPECT_FALSE(result.usedFallback);
    EXPECT_EQ(auth.starts, 2);
    EXPECT_EQ(auth.lastOperation, AuthOperation::Send);
    EXPECT_EQ(auth.lastStream, QStringLiteral("1001"));
    EXPECT_EQ(auth.lastText, QStringLiteral("one destination"));
    EXPECT_TRUE(twitch->sent.empty());
    EXPECT_TRUE(kick->sent.empty());
    EXPECT_EQ(callbacks, 0);
    auth.complete(200,
                  QByteArrayLiteral(R"({"data":{"id":"release-message"}})"));
    EXPECT_EQ(callbacks, 1);
    ASSERT_TRUE(completedRoute);
    EXPECT_EQ(completedRoute->destinationIndex, result.destinationIndex);
    EXPECT_EQ(completedRoute->destination.get(), result.destination.get());
    EXPECT_EQ(completedRoute->destinationPlatform, result.destinationPlatform);
    EXPECT_EQ(completedRoute->destinationName, result.destinationName);
    EXPECT_EQ(completedRoute->evaluations.size(), result.evaluations.size());
    EXPECT_EQ(auth.starts, 2);
    EXPECT_TRUE(twitch->sent.empty());
    EXPECT_TRUE(kick->sent.empty());

    multi.reset();
    session->clear();
    session.reset();
    fixture.controller->beginShutdown();
    fixture.controller->shutdown();
    fixture.dispatcher->runAll();
    fixture.manual.runUntilIdle();
    EXPECT_EQ(rumble->state(), RumbleChannelState::Closed);
    EXPECT_EQ(fixture.transport.activeRequestCount(), 0U);
    EXPECT_EQ(fixture.scheduler.activeTaskCount(), 0U);
}
