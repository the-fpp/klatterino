// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT
#include "providers/rumble/RumbleChannel.hpp"

#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageFlag.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Logging.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleEvent.hpp"
#include "providers/rumble/RumbleSession.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chatterino {
namespace {
class NoopAuthHandle final : public rumble::AuthHandle
{
public:
    void cancel() noexcept override
    {
        cancelled = true;
    }
    bool cancelled = false;
};

class DeferredChannelAuthTransport final : public rumble::AuthTransport
{
public:
    std::unique_ptr<rumble::AuthHandle> start(
        rumble::AuthOperation operation, QString, QString, QByteArray,
        QByteArray, rumble::AuthCallbacks callbacks) override
    {
        if (operation == rumble::AuthOperation::Probe)
        {
            callbacks.complete({200, "application/json", probeResponse, {}});
        }
        else
        {
            if (operation == rumble::AuthOperation::Send)
                ++sendStarts;
            pending = std::move(callbacks);
        }
        return std::make_unique<NoopAuthHandle>();
    }
    QByteArray probeResponse = R"({"user":{"id":"channel-test"}})";
    rumble::AuthCallbacks pending;
    int sendStarts = 0;
};

class MockApplication : public mock::BaseApplication
{
public:
    EmoteController *getEmotes() override
    {
        return &emotes;
    }
    ILogging *getChatLogger() override
    {
        return &logging;
    }
    mock::EmoteController emotes;
    mock::EmptyLogging logging;
};
class RoutedTestChannel final : public Channel
{
public:
    RoutedTestChannel()
        : Channel(QStringLiteral("fallback"), Type::Twitch)
    {
    }

    bool canSendMessage() const override
    {
        return true;
    }

    bool isWritable() const override
    {
        return true;
    }

    MessageSendContext messageSendContext() const override
    {
        return {
            .platform = QStringLiteral("twitch"),
            .channelID = getName(),
            .accountID = QStringLiteral("twitch-test"),
            .writable = true,
            .authenticated = true,
        };
    }

    void sendMessageAsync(QString message, SendCallback callback) override
    {
        sent.push_back(std::move(message));
        if (callback)
        {
            callback({SendOutcome::Confirmed, {}});
        }
    }

    std::vector<QString> sent;
};
class FifoDispatcher final : public RumbleDispatcher
{
public:
    FifoDispatcher()
        : ownerThreadId(std::this_thread::get_id())
    {
    }
    bool isOwnerThread() const noexcept override
    {
        return ownerThread.load(std::memory_order_acquire) &&
               std::this_thread::get_id() == ownerThreadId;
    }
    bool dispatch(Task task) override
    {
        if (!acceptTasks.load(std::memory_order_acquire))
            return false;
        std::lock_guard lock(tasksMutex);
        tasks.emplace_back(std::move(task));
        return true;
    }
    void dispose(Task cleanup) noexcept override
    {
        auto run = [this, cleanup = std::move(cleanup)]() mutable {
            disposalThread = std::this_thread::get_id();
            ++disposalCount;
            cleanup();
        };
        if (isOwnerThread())
        {
            run();
            return;
        }
        auto keepAlive = shared_from_this();
        std::lock_guard lock(tasksMutex);
        tasks.emplace_back(
            [keepAlive = std::move(keepAlive), run = std::move(run)]() mutable {
                run();
            });
    }
    void runAll()
    {
        ASSERT_TRUE(isOwnerThread());
        while (true)
        {
            auto task = takeNext();
            if (!task)
                break;
            task();
        }
    }
    Task takeNext()
    {
        std::lock_guard lock(tasksMutex);
        if (tasks.empty())
            return {};
        auto task = std::move(tasks.front());
        tasks.pop_front();
        return task;
    }
    void discardAll()
    {
        ASSERT_TRUE(isOwnerThread());
        std::deque<Task> discarded;
        {
            std::lock_guard lock(tasksMutex);
            discarded.swap(tasks);
        }
    }
    std::size_t pending() const
    {
        std::lock_guard lock(tasksMutex);
        return tasks.size();
    }
    std::atomic_bool ownerThread{true};
    std::atomic_bool acceptTasks{true};
    const std::thread::id ownerThreadId;
    mutable std::mutex tasksMutex;
    std::deque<Task> tasks;
    std::thread::id disposalThread;
    int disposalCount = 0;
};
class FakeOperation final : public RumbleChannelOperation
{
public:
    explicit FakeOperation(
        std::shared_ptr<int> cancelCount,
        std::shared_ptr<int> releaseCount = {},
        std::shared_ptr<std::thread::id> cancelThread = {},
        std::shared_ptr<int> destructionCount = {},
        std::shared_ptr<std::thread::id> destructionThread = {})
        : cancelCount_(std::move(cancelCount))
        , releaseCount_(std::move(releaseCount))
        , cancelThread_(std::move(cancelThread))
        , destructionCount_(std::move(destructionCount))
        , destructionThread_(std::move(destructionThread))
    {
    }
    ~FakeOperation() override
    {
        cancel();
        if (destructionCount_)
            ++*destructionCount_;
        if (destructionThread_)
            *destructionThread_ = std::this_thread::get_id();
    }
    void cancel() noexcept override
    {
        if (!std::exchange(finalized_, true))
        {
            ++*cancelCount_;
            if (cancelThread_)
                *cancelThread_ = std::this_thread::get_id();
        }
    }
    void release() noexcept override
    {
        if (!std::exchange(finalized_, true) && releaseCount_)
            ++*releaseCount_;
    }

private:
    std::shared_ptr<int> cancelCount_;
    std::shared_ptr<int> releaseCount_;
    std::shared_ptr<std::thread::id> cancelThread_;
    std::shared_ptr<int> destructionCount_;
    std::shared_ptr<std::thread::id> destructionThread_;
    bool finalized_ = false;
};
class DestructionThreadProbe
{
public:
    DestructionThreadProbe(std::shared_ptr<int> count,
                           std::shared_ptr<std::thread::id> thread)
        : count_(std::move(count))
        , thread_(std::move(thread))
    {
    }
    ~DestructionThreadProbe()
    {
        ++*count_;
        *thread_ = std::this_thread::get_id();
    }

private:
    std::shared_ptr<int> count_;
    std::shared_ptr<std::thread::id> thread_;
};
class CallbackOperation final : public RumbleChannelOperation
{
public:
    explicit CallbackOperation(std::function<void()> onCancel)
        : onCancel_(std::move(onCancel))
    {
    }
    ~CallbackOperation() override
    {
        cancel();
    }
    void cancel() noexcept override
    {
        if (!std::exchange(finalized_, true) && onCancel_)
            onCancel_();
    }
    void release() noexcept override
    {
        finalized_ = true;
    }

private:
    std::function<void()> onCancel_;
    bool finalized_ = false;
};
class DestructionCallbackProbe
{
public:
    explicit DestructionCallbackProbe(std::function<void()> callback)
        : callback_(std::move(callback))
    {
    }
    ~DestructionCallbackProbe()
    {
        callback_();
    }

private:
    std::function<void()> callback_;
};
RumbleChannelKey makeKey(RumbleChannelKeyKind kind, QString value)
{
    auto result = RumbleChannelKey::normalize(kind, std::move(value));
    EXPECT_TRUE(result);
    return *result;
}
std::shared_ptr<RumbleChannel> makeChannel(RumbleChannelProvider &provider,
                                           QString slug)
{
    auto result = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                       std::move(slug));
    EXPECT_TRUE(result);
    return *result;
}
RumbleMessagePublication makePublication(QString id, bool rant = false)
{
    auto message = std::make_shared<Message>();
    message->id = std::move(id);
    message->platform = MessagePlatform::Rumble;
    auto result = RumbleMessagePublication::create(message, rant);
    EXPECT_TRUE(result);
    return *result;
}
RumbleMessagePublication makeTimedPublication(QString id, qint64 epochMs,
                                              bool rant = false)
{
    auto message = std::make_shared<Message>();
    message->id = std::move(id);
    message->platform = MessagePlatform::Rumble;
    message->serverReceivedTime =
        QDateTime::fromMSecsSinceEpoch(epochMs, Qt::UTC);
    auto result = RumbleMessagePublication::create(message, rant);
    EXPECT_TRUE(result);
    return *result;
}
RumbleMessageId makeMessageId(QString id)
{
    auto result = RumbleMessageId::fromNormalized(std::move(id));
    EXPECT_TRUE(result);
    return *result;
}
RumbleFailure makeFailure()
{
    return {RumbleFailureCategory::Transport, RumbleFailureCode::Timeout,
            RumbleOperatorText::RequestTimedOut};
}
void reachState(RumbleChannel &channel, RumbleChannelState state)
{
    using S = RumbleChannelState;
    switch (state)
    {
        case S::Unresolved:
            return;
        case S::Offline:
            EXPECT_TRUE(channel.transitionTo(S::Offline));
            return;
        case S::Connecting:
            EXPECT_TRUE(channel.transitionTo(S::Connecting));
            return;
        case S::Connected:
            EXPECT_TRUE(channel.transitionTo(S::Connecting));
            EXPECT_TRUE(channel.transitionTo(S::Connected));
            return;
        case S::Backoff:
            EXPECT_TRUE(channel.transitionTo(S::Backoff));
            return;
        case S::Failed:
            EXPECT_TRUE(channel.transitionTo(S::Failed, makeFailure()));
            return;
        case S::Closed:
            channel.close();
            return;
    }
}
bool expectedTransition(RumbleChannelState from, RumbleChannelState to)
{
    using S = RumbleChannelState;
    if (from == to)
        return true;
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
}  // namespace

TEST(RumbleChannelKey, NormalizesKindsWithoutCrossKindCollision)
{
    auto slug = RumbleChannelKey::normalize(RumbleChannelKeyKind::ChannelSlug,
                                            QStringLiteral("  Foo-Bar  "));
    auto embed = RumbleChannelKey::normalize(RumbleChannelKeyKind::EmbedId,
                                             QStringLiteral(" V0Az "));
    auto stream = RumbleChannelKey::normalize(RumbleChannelKeyKind::StreamId,
                                              QStringLiteral(" 0000042 "));
    ASSERT_TRUE(slug);
    ASSERT_TRUE(embed);
    ASSERT_TRUE(stream);
    EXPECT_EQ(slug->value(), QStringLiteral("foo-bar"));
    EXPECT_EQ(embed->value(), QStringLiteral("v0az"));
    EXPECT_EQ(stream->value(), QStringLiteral("42"));
    const auto sameSlug =
        makeKey(RumbleChannelKeyKind::ChannelSlug, QStringLiteral("FOO-BAR"));
    const auto sameEmbed =
        makeKey(RumbleChannelKeyKind::EmbedId, QStringLiteral("v0az"));
    const auto sameStream =
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("42"));
    EXPECT_EQ(*slug, sameSlug);
    EXPECT_EQ(*embed, sameEmbed);
    EXPECT_EQ(*stream, sameStream);
    const RumbleChannelKeyHash hash;
    EXPECT_EQ(hash(*slug), hash(sameSlug));
    EXPECT_EQ(hash(*embed), hash(sameEmbed));
    EXPECT_EQ(hash(*stream), hash(sameStream));
    auto slugV1 =
        makeKey(RumbleChannelKeyKind::ChannelSlug, QStringLiteral("v1"));
    auto embedV1 = makeKey(RumbleChannelKeyKind::EmbedId, QStringLiteral("v1"));
    EXPECT_NE(slugV1, embedV1);
    std::unordered_set<RumbleChannelKey, RumbleChannelKeyHash> keys;
    keys.emplace(slugV1);
    keys.emplace(embedV1);
    EXPECT_EQ(keys.size(), 2U);
    EXPECT_EQ(magic_enum::enum_name(Channel::Type::Rumble), "rumble");
}
TEST(RumbleChannelKey, RejectsEmptyMalformedAndZeroKeys)
{
    auto empty = RumbleChannelKey::normalize(RumbleChannelKeyKind::ChannelSlug,
                                             QStringLiteral(" "));
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error(), RumbleChannelKeyError::Empty);
    auto slash = RumbleChannelKey::normalize(RumbleChannelKeyKind::ChannelSlug,
                                             QStringLiteral("bad/path"));
    ASSERT_FALSE(slash);
    EXPECT_EQ(slash.error(), RumbleChannelKeyError::InvalidSlug);
    for (const auto &value : {QStringLiteral("x1"), QStringLiteral("v1-")})
    {
        auto result =
            RumbleChannelKey::normalize(RumbleChannelKeyKind::EmbedId, value);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), RumbleChannelKeyError::InvalidEmbedId);
    }
    for (const auto &value : {QStringLiteral("0"), QStringLiteral("000"),
                              QStringLiteral("12a"), QStringLiteral("-1")})
    {
        auto result =
            RumbleChannelKey::normalize(RumbleChannelKeyKind::StreamId, value);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), RumbleChannelKeyError::InvalidStreamId);
    }
}
TEST(RumbleChannelProvider, SharesLiveIdentityAndRecreatesExpiredEntries)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    std::weak_ptr<RumbleChannel> expired;
    {
        auto first = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                          QStringLiteral(" Alpha "));
        auto second = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                           QStringLiteral("alpha"));
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_EQ(*first, *second);
        expired = *first;
    }
    ASSERT_TRUE(expired.expired());
    auto replacement = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                            QStringLiteral("ALPHA"));
    ASSERT_TRUE(replacement);
    std::weak_ptr<RumbleChannel> replacementWeak = *replacement;
    EXPECT_TRUE(expired.owner_before(replacementWeak) ||
                replacementWeak.owner_before(expired));
}

TEST(RumbleChannelCapability,
     ObservedMessageLimitIsBoundedAndInvalidatedByConnectionGeneration)
{
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto channel = makeChannel(provider, QStringLiteral("limit-test"));
    auto first = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(first);
    channel->publishMessageLengthMax(*first, 200);
    EXPECT_EQ(channel->messageSendContext().maxMessageLength, 200);

    channel->publishMessageLengthMax(*first, 1000000);
    EXPECT_EQ(channel->messageSendContext().maxMessageLength,
              rumble::SessionController::ABSOLUTE_TEXT_LIMIT);

    auto second = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(second);
    EXPECT_EQ(channel->messageSendContext().maxMessageLength,
              rumble::SessionController::ABSOLUTE_TEXT_LIMIT);
    channel->publishMessageLengthMax(*first, 50);
    EXPECT_EQ(channel->messageSendContext().maxMessageLength,
              rumble::SessionController::ABSOLUTE_TEXT_LIMIT);
}

TEST(RumbleChannelCapability,
     NativeEmotesRequireValidatedAccountAndExactDestinationEligibility)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto channel = makeChannel(provider, QStringLiteral("emote-capability"));
    DeferredChannelAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession("SYNTHETIC_SESSION_CANARY"));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    channel->setSessionController(session);

    auto token = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Channel"), std::nullopt, std::nullopt,
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("42")));
    ASSERT_TRUE(metadata);
    channel->publishMetadata(*token, std::move(*metadata));
    channel->publishLifecycle(*token, RumbleChannelState::Connecting, {});
    channel->publishLifecycle(*token, RumbleChannelState::Connected, {});
    ASSERT_TRUE(transport.pending.complete);
    channel->publishEmoteCatalog(
        *token,
        {.emotes = {
             {.id = QStringLiteral("global:1"),
              .name = QStringLiteral("r+global"),
              .imageUrl =
                  QStringLiteral("https://1a-1791.com/video/z12/global.png")},
             {.id = QStringLiteral("channel:2"),
              .name = QStringLiteral("Channel"),
              .imageUrl =
                  QStringLiteral("https://1a-1791.com/video/z12/channel.png"),
              .scope = rumble::EmoteScope::Channel},
             {.id = QStringLiteral("channel:3"),
              .name = QStringLiteral("Subscriber"),
              .imageUrl = QStringLiteral(
                  "https://1a-1791.com/video/z12/subscriber.png"),
              .scope = rumble::EmoteScope::Channel,
              .subscribersOnly = true},
         }});

    auto available = channel->availableEmotes();
    ASSERT_EQ(available.size(), 1U);
    EXPECT_EQ(available[0].insertionText, QStringLiteral(":r+global:"));

    transport.pending.complete(
        {200,
         "text/event-stream",
         "data: "
         "{\"type\":\"init\",\"data\":{\"users\":[{\"id\":\"channel-test\","
         "\"is_follower\":true,\"badges\":[]}]}}\n\n",
         {}});
    available = channel->availableEmotes();
    ASSERT_EQ(available.size(), 2U);
    EXPECT_EQ(available[1].insertionText, QStringLiteral(":Channel:"));

    completion::EmoteSource completion(
        channel.get(), std::make_unique<completion::ClassicEmoteStrategy>());
    completion.update(QString{});
    EXPECT_EQ(std::ranges::count_if(completion.output(),
                                    [](const auto &candidate) {
                                        return candidate.identity.provider ==
                                               QStringLiteral("rumble");
                                    }),
              2);

    const auto context = channel->messageSendContext();
    EXPECT_TRUE(context.emoteCapabilitiesComplete);
    ASSERT_EQ(context.emoteCapabilities.size(), 2U);
    EXPECT_EQ(context.emoteCapabilities[0].identity.provider,
              QStringLiteral("rumble"));
    EXPECT_FALSE(context.emoteCapabilities[0].availability.channelID);
    ASSERT_TRUE(context.emoteCapabilities[0].availability.accountID);
    EXPECT_EQ(*context.emoteCapabilities[0].availability.accountID,
              QStringLiteral("channel-test"));
    ASSERT_TRUE(context.emoteCapabilities[1].availability.channelID);
    EXPECT_EQ(*context.emoteCapabilities[1].availability.channelID,
              context.channelID);

    session->clear();
    EXPECT_TRUE(channel->availableEmotes().empty());
    ASSERT_TRUE(session->importSession("SYNTHETIC_SECOND_SESSION"));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    channel->setSessionController(session);
    ASSERT_TRUE(transport.pending.complete);
    transport.pending.complete(
        {200,
         "text/event-stream",
         "data: "
         "{\"type\":\"init\",\"data\":{\"users\":[{\"id\":\"channel-test\","
         "\"is_follower\":false,\"badges\":[\"locals_supporter\"]}]}}\n\n",
         {}});
    EXPECT_EQ(channel->availableEmotes().size(), 3U);
    completion.update(QString{});
    EXPECT_EQ(std::ranges::count_if(completion.output(),
                                    [](const auto &candidate) {
                                        return candidate.identity.provider ==
                                               QStringLiteral("rumble");
                                    }),
              3);

    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Offline));
    EXPECT_FALSE(channel->messageSendContext().writable);
    EXPECT_TRUE(channel->messageSendContext().emoteCapabilities.empty());
    completion.update(QString{});
    EXPECT_TRUE(
        std::ranges::none_of(completion.output(), [](const auto &candidate) {
            return candidate.identity.provider == QStringLiteral("rumble");
        }));

    session->clear();
    EXPECT_TRUE(channel->messageSendContext().emoteCapabilities.empty());
    completion.update(QString{});
    EXPECT_TRUE(
        std::ranges::none_of(completion.output(), [](const auto &candidate) {
            return candidate.identity.provider == QStringLiteral("rumble");
        }));
}

TEST(RumbleChannelState,
     ConfirmedLiveSurvivesTransportBackoffUntilOfflineEvidence)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto channel = makeChannel(provider, QStringLiteral("availability"));
    std::vector<bool> liveValues;
    int writabilityChanges = 0;
    std::ignore = channel->liveStatusChanged.connect([&] {
        liveValues.push_back(channel->isLive());
    });
    std::ignore = channel->writabilityChanged.connect([&] {
        ++writabilityChanges;
    });

    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connected));
    EXPECT_TRUE(channel->isLive());
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Backoff));
    EXPECT_TRUE(channel->isLive());
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Unresolved));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
    EXPECT_TRUE(channel->isLive());
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Offline));
    EXPECT_FALSE(channel->isLive());

    EXPECT_EQ(liveValues, (std::vector<bool>{true, false}));
    EXPECT_EQ(writabilityChanges, 2);
}

TEST(RumbleChannelCapability, ConfirmedOfflineRejectsSendBeforeTransport)
{
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto channel = makeChannel(provider, QStringLiteral("offline-send"));
    DeferredChannelAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession("SYNTHETIC_SESSION_CANARY"));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    channel->setSessionController(session);

    auto token = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Channel"), std::nullopt, std::nullopt,
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("42")));
    ASSERT_TRUE(metadata);
    channel->publishMetadata(*token, std::move(*metadata));
    channel->publishLifecycle(*token, RumbleChannelState::Connecting, {});
    channel->publishLifecycle(*token, RumbleChannelState::Offline, {});

    std::optional<Channel::SendResult> result;
    channel->sendMessageAsync(QStringLiteral("must not dispatch"),
                              [&](Channel::SendResult value) {
                                  result = std::move(value);
                              });
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Channel::SendOutcome::DefiniteFailure);
    EXPECT_EQ(transport.sendStarts, 0);

    auto loggedOut =
        makeChannel(provider, QStringLiteral("offline-logged-out"));
    ASSERT_TRUE(loggedOut->transitionTo(RumbleChannelState::Offline));
    result.reset();
    loggedOut->sendMessageAsync(QStringLiteral("must remain offline"),
                                [&](Channel::SendResult value) {
                                    result = std::move(value);
                                });
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Channel::SendOutcome::DefiniteFailure);
    EXPECT_TRUE(result->userMessage.contains(QStringLiteral("unavailable")));
}

TEST(RumbleChannelCapability,
     StreamGenerationChangeSuppressesOutstandingSendCompletion)
{
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto channel = makeChannel(provider, QStringLiteral("send-generation"));
    DeferredChannelAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession("SYNTHETIC_SESSION_CANARY"));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    channel->setSessionController(session);

    auto token = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Channel"), std::nullopt, std::nullopt,
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("42")));
    ASSERT_TRUE(metadata);
    channel->publishMetadata(*token, std::move(*metadata));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connected));

    bool completed = false;
    channel->sendMessageAsync(QStringLiteral("hello"),
                              [&](Channel::SendResult) {
                                  completed = true;
                              });
    ASSERT_TRUE(transport.pending.complete);

    auto replacement = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(replacement);
    transport.pending.complete(
        {200, "application/json", R"({"data":{"id":"stale"}})", {}});
    EXPECT_FALSE(completed);
}

TEST(RumbleChannelCapability,
     ConfirmedSendEchoAndAuthoritativeEventReconcileInEitherOrder)
{
    for (const int scenario : {0, 1, 2})
    {
        const bool responseFirst = scenario != 0;
        const bool bootstrap = scenario == 2;
        SCOPED_TRACE(bootstrap       ? "response-first-bootstrap"
                     : responseFirst ? "response-first-realtime"
                                     : "event-first-realtime");
        MockApplication app;
        auto dispatcher = std::make_shared<FifoDispatcher>();
        RumbleChannelProvider provider(dispatcher);
        auto channel = makeChannel(provider, QStringLiteral("send-echo"));
        DeferredChannelAuthTransport transport;
        transport.probeResponse =
            R"({"user":{"id":"channel-test","username":"decorated-user"}})";
        auto session = std::make_shared<rumble::SessionController>(transport);
        ASSERT_TRUE(session->importSession("SYNTHETIC_SESSION_CANARY"));
        session->validate({});
        ASSERT_EQ(session->state(), rumble::SessionState::Valid);
        channel->setSessionController(session);

        auto token = channel->beginOperation(RumbleOperationKind::Connection);
        ASSERT_TRUE(token);
        auto metadata = RumbleResolvedMetadata::create(
            QStringLiteral("Synthetic channel"), std::nullopt, std::nullopt,
            makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("42")));
        ASSERT_TRUE(metadata);
        channel->publishMetadata(*token, std::move(*metadata));
        ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
        ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connected));

        size_t appended = 0;
        size_t replaced = 0;
        std::ignore = channel->messageAppended.connect([&](auto &, auto) {
            ++appended;
        });
        std::ignore = channel->messageReplaced.connect(
            [&](size_t, const auto &, const auto &) {
                ++replaced;
            });

        std::optional<Channel::SendResult> sendResult;
        channel->sendMessageAsync(QStringLiteral("native :r+fixture: content"),
                                  [&](Channel::SendResult result) {
                                      sendResult = std::move(result);
                                  });
        ASSERT_TRUE(transport.pending.complete);

        rumble::MessageDto authoritative{
            .id = QStringLiteral("message-1"),
            .userId = QStringLiteral("channel-test"),
            .channelId = QStringLiteral("42"),
            .text = QStringLiteral("native :r+fixture: content"),
            .createdOn = QStringLiteral("2026-07-21T00:00:00.000Z"),
            .timestamp = QDateTime::fromString(
                QStringLiteral("2026-07-21T00:00:00.000Z"), Qt::ISODateWithMs),
            .loginName = QStringLiteral("decorated-user"),
            .displayName = QStringLiteral("Decorated User"),
            .color = QColor(0, 200, 80),
            .channelName = QStringLiteral("Synthetic channel"),
            .badgeIds = QStringList{QStringLiteral("verified")},
            .roleIds = QStringList{QStringLiteral("moderator")},
            .source = QStringLiteral("chat"),
        };
        auto publication = RumbleMessagePublication::fromDto(authoritative);
        ASSERT_TRUE(publication);

        if (responseFirst)
        {
            transport.pending.complete({200,
                                        "application/json",
                                        R"({"data":{"id":"message-1"}})",
                                        {}});
            ASSERT_TRUE(sendResult);
            EXPECT_EQ(sendResult->outcome, Channel::SendOutcome::Confirmed);
            auto optimistic =
                channel->findMessageByID(QStringLiteral("message-1"));
            ASSERT_NE(optimistic, nullptr);
            EXPECT_EQ(optimistic->platform, MessagePlatform::Rumble);
            EXPECT_TRUE(
                optimistic->flags.has(MessageFlag::DoNotTriggerNotification));
            ASSERT_GE(optimistic->elements.size(), 2U);
            auto *username =
                dynamic_cast<TextElement *>(optimistic->elements[1].get());
            ASSERT_NE(username, nullptr);
            EXPECT_TRUE(
                username->getFlags().has(MessageElementFlag::RumbleUsername));
            EXPECT_FALSE(
                username->getFlags().has(MessageElementFlag::KickUsername));

            if (bootstrap)
            {
                std::vector<RumbleMessagePublication> batch;
                batch.emplace_back(std::move(*publication));
                channel->publishBootstrap(*token, std::move(batch));
            }
            else
            {
                channel->publishRealtime(*token, std::move(*publication));
            }
            EXPECT_EQ(appended, 1U);
            EXPECT_EQ(replaced, 1U);
        }
        else
        {
            channel->publishRealtime(*token, std::move(*publication));
            auto beforeResponse =
                channel->findMessageByID(QStringLiteral("message-1"));
            ASSERT_NE(beforeResponse, nullptr);
            transport.pending.complete({200,
                                        "application/json",
                                        R"({"data":{"id":"message-1"}})",
                                        {}});
            EXPECT_EQ(channel->findMessageByID(QStringLiteral("message-1")),
                      beforeResponse);
            EXPECT_EQ(appended, 1U);
            EXPECT_EQ(replaced, 0U);
        }

        ASSERT_TRUE(sendResult);
        EXPECT_EQ(sendResult->outcome, Channel::SendOutcome::Confirmed);
        const auto snapshot = channel->getMessageSnapshot();
        ASSERT_EQ(snapshot.size(), 1U);
        const auto &resolved = snapshot.front();
        EXPECT_EQ(resolved->id, QStringLiteral("message-1"));
        EXPECT_EQ(resolved->platform, MessagePlatform::Rumble);
        EXPECT_EQ(resolved->usernameColor, QColor(0, 200, 80));
        ASSERT_TRUE(resolved->rumble);
        EXPECT_EQ(resolved->rumble->badgeIDs,
                  QStringList{QStringLiteral("verified")});
        EXPECT_EQ(resolved->rumble->roleIDs,
                  QStringList{QStringLiteral("moderator")});
        ASSERT_GE(resolved->elements.size(), 2U);
        auto *resolvedUsername =
            dynamic_cast<TextElement *>(resolved->elements[1].get());
        ASSERT_NE(resolvedUsername, nullptr);
        EXPECT_TRUE(resolvedUsername->getFlags().has(
            MessageElementFlag::RumbleUsername));
        EXPECT_FALSE(
            resolvedUsername->getFlags().has(MessageElementFlag::KickUsername));
    }
}

TEST(RumbleChannelCapability,
     AutomaticAndExplicitMultiChannelRoutesUseRumbleEchoStyling)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto rumbleChannel =
        makeChannel(provider, QStringLiteral("multi-send-echo"));
    auto twitchChannel = std::make_shared<RoutedTestChannel>();
    DeferredChannelAuthTransport transport;
    transport.probeResponse =
        R"({"user":{"id":"channel-test","username":"decorated-user"}})";
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession("SYNTHETIC_SESSION_CANARY"));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    rumbleChannel->setSessionController(session);

    auto token = rumbleChannel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Synthetic channel"), std::nullopt, std::nullopt,
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("42")));
    ASSERT_TRUE(metadata);
    rumbleChannel->publishMetadata(*token, std::move(*metadata));
    ASSERT_TRUE(rumbleChannel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(rumbleChannel->transitionTo(RumbleChannelState::Connected));

    using Platform = MultiChannel::Platform;
    const std::array specs{
        MultiChannel::Spec{
            .platform = Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/c/synthetic")},
        MultiChannel::Spec{.platform = Platform::Twitch,
                           .name = QStringLiteral("fallback")},
    };
    auto multi = std::make_shared<MultiChannel>(
        std::span{specs}, MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [&](const MultiChannel::Spec &spec) -> ChannelPtr {
            if (spec.platform == Platform::Rumble)
            {
                return rumbleChannel;
            }
            return twitchChannel;
        });

    const auto expectRumbleEcho = [&](QStringView id) {
        auto message = rumbleChannel->findMessageByID(id);
        ASSERT_NE(message, nullptr);
        EXPECT_EQ(message->platform, MessagePlatform::Rumble);
        ASSERT_GE(message->elements.size(), 2U);
        auto *username =
            dynamic_cast<TextElement *>(message->elements[1].get());
        ASSERT_NE(username, nullptr);
        EXPECT_TRUE(
            username->getFlags().has(MessageElementFlag::RumbleUsername));
        EXPECT_FALSE(
            username->getFlags().has(MessageElementFlag::KickUsername));
    };

    auto automaticDraft =
        MessageDraft::fromPlainText(QStringLiteral("automatic route"));
    auto automatic = multi->sendMessageDraftAsync(
        automaticDraft, automaticDraft.text, 0,
        MultiChannelRoutePolicy::CompatibleFallback, {});
    ASSERT_EQ(automatic.destination, rumbleChannel);
    ASSERT_TRUE(transport.pending.complete);
    transport.pending.complete(
        {200, "application/json", R"({"data":{"id":"automatic-id"}})", {}});
    expectRumbleEcho(QStringLiteral("automatic-id"));
    EXPECT_TRUE(twitchChannel->sent.empty());

    multi->setActiveChannelIndex(1);
    auto overrideDraft =
        MessageDraft::fromPlainText(QStringLiteral("explicit route"));
    overrideDraft.destinationPlatformOverride = QStringLiteral("rumble");
    auto overridden = multi->sendMessageDraftAsync(
        overrideDraft, overrideDraft.text, 1,
        MultiChannelRoutePolicy::CompatibleFallback, {});
    ASSERT_EQ(overridden.destination, rumbleChannel);
    ASSERT_TRUE(transport.pending.complete);
    transport.pending.complete(
        {200, "application/json", R"({"data":{"id":"override-id"}})", {}});
    expectRumbleEcho(QStringLiteral("override-id"));
    EXPECT_TRUE(twitchChannel->sent.empty());
    EXPECT_EQ(rumbleChannel->getMessageSnapshot().size(), 2U);
}
TEST(RumbleChannelProvider, BaseIdentityIsOpaqueSafeAndStableAcrossAliases)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);

    auto slug42 = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                       QStringLiteral("42"));
    auto stream42 = provider.getOrCreate(RumbleChannelKeyKind::StreamId,
                                         QStringLiteral("42"));
    auto slugV1 = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                       QStringLiteral("v1"));
    auto embedV1 = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("v1"));
    ASSERT_TRUE(slug42);
    ASSERT_TRUE(stream42);
    ASSERT_TRUE(slugV1);
    ASSERT_TRUE(embedV1);
    EXPECT_NE(*slug42, *stream42);
    EXPECT_NE((*slug42)->getName(), (*stream42)->getName());
    EXPECT_NE((*slug42)->messageSendContext().channelID,
              (*stream42)->messageSendContext().channelID);
    EXPECT_NE(*slugV1, *embedV1);
    EXPECT_NE((*slugV1)->getName(), (*embedV1)->getName());
    EXPECT_NE((*slugV1)->messageSendContext().channelID,
              (*embedV1)->messageSendContext().channelID);

    const auto permanentIdentity = (*stream42)->getName();
    EXPECT_NE(permanentIdentity, (*stream42)->key().value());
    EXPECT_EQ((*stream42)->messageSendContext().channelID, permanentIdentity);
    ASSERT_TRUE(
        provider.associateAlias(*stream42, RumbleChannelKeyKind::ChannelSlug,
                                QStringLiteral("canonical-stream-channel")));
    EXPECT_EQ((*stream42)->getName(), permanentIdentity);
    EXPECT_EQ((*stream42)->messageSendContext().channelID, permanentIdentity);

    // A new channel object receives a fresh identity even when it starts from
    // the same locator. Locator values and creation order cannot become the
    // permanent logger/draft identity.
    RumbleChannelProvider laterProvider(dispatcher);
    auto unrelated = makeChannel(laterProvider, QStringLiteral("unrelated"));
    auto sameStreamLater = laterProvider.getOrCreate(
        RumbleChannelKeyKind::StreamId, QStringLiteral("00042"));
    ASSERT_TRUE(unrelated);
    ASSERT_TRUE(sameStreamLater);
    EXPECT_NE((*sameStreamLater)->getName(), permanentIdentity);
    EXPECT_NE(unrelated->getName(), permanentIdentity);
    EXPECT_TRUE(permanentIdentity.startsWith(QStringLiteral("rumble-")));
    EXPECT_EQ(permanentIdentity.size(), 39);
    for (const auto character : permanentIdentity.sliced(7))
    {
        EXPECT_TRUE((character >= u'0' && character <= u'9') ||
                    (character >= u'a' && character <= u'f'));
    }
}
TEST(RumbleChannelProvider, AliasesPruneAndRejectLiveConflicts)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto alpha = makeChannel(provider, QStringLiteral("alpha"));
    auto bravo = makeChannel(provider, QStringLiteral("bravo"));
    ASSERT_TRUE(provider.associateAlias(alpha, RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("V123")));
    auto alias = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                      QStringLiteral("v123"));
    ASSERT_TRUE(alias);
    EXPECT_EQ(*alias, alpha);
    EXPECT_TRUE(provider.associateAlias(alpha, RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("v123")));
    auto conflict = provider.associateAlias(
        bravo, RumbleChannelKeyKind::EmbedId, QStringLiteral("v123"));
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, RumbleProviderErrorCode::AliasConflict);
    std::weak_ptr<RumbleChannel> expired;
    {
        auto temporary = makeChannel(provider, QStringLiteral("temporary"));
        ASSERT_TRUE(provider.associateAlias(
            temporary, RumbleChannelKeyKind::EmbedId, QStringLiteral("vdead")));
        expired = temporary;
    }
    ASSERT_TRUE(expired.expired());
    auto recreated = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                          QStringLiteral("vdead"));
    ASSERT_TRUE(recreated);
    EXPECT_NE((*recreated)->state(), RumbleChannelState::Closed);
}
TEST(RumbleChannelProvider, InvalidLookupWrongThreadAndShutdownAreTyped)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto invalid = provider.getOrCreate(RumbleChannelKeyKind::StreamId,
                                        QStringLiteral("000"));
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, RumbleProviderErrorCode::InvalidKey);
    EXPECT_EQ(invalid.error().keyError, RumbleChannelKeyError::InvalidStreamId);
    dispatcher->ownerThread = false;
    auto wrongThread = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                            QStringLiteral("offthread"));
    ASSERT_FALSE(wrongThread);
    EXPECT_EQ(wrongThread.error().code, RumbleProviderErrorCode::WrongThread);
    EXPECT_EQ(dispatcher->pending(), 0U);
    dispatcher->ownerThread = true;
    auto live = makeChannel(provider, QStringLiteral("alpha"));
    provider.shutdown();
    EXPECT_EQ(live->state(), RumbleChannelState::Closed);
    provider.shutdown();
    auto after = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                      QStringLiteral("new"));
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code, RumbleProviderErrorCode::Shutdown);
}
TEST(RumbleChannelState, ImplementsEveryAllowedAndRejectedTransition)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    constexpr std::array states{
        RumbleChannelState::Unresolved, RumbleChannelState::Offline,
        RumbleChannelState::Connecting, RumbleChannelState::Connected,
        RumbleChannelState::Backoff,    RumbleChannelState::Failed,
        RumbleChannelState::Closed,
    };
    int sequence = 0;
    for (const auto from : states)
    {
        for (const auto to : states)
        {
            auto item = makeChannel(provider,
                                    QStringLiteral("state-%1").arg(sequence++));
            reachState(*item, from);
            const auto before = item->state();
            auto result = item->transitionTo(
                to, to == RumbleChannelState::Failed
                        ? std::optional<RumbleFailure>(makeFailure())
                        : std::nullopt);
            const bool expected = expectedTransition(from, to);
            EXPECT_EQ(static_cast<bool>(result), expected);
            EXPECT_EQ(item->state(), expected ? to : before);
        }
    }
}
TEST(RumbleChannelState, SignalsObserveCommittedValuesInStableOrder)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("signals"));
    std::vector<std::string> events;
    std::ignore = item->stateChanged.connect(
        [&](RumbleChannelState previous, RumbleChannelState current) {
            EXPECT_EQ(item->state(), current);
            EXPECT_NE(previous, current);
            events.emplace_back("state");
        });
    std::ignore = item->liveStatusChanged.connect([&] {
        EXPECT_EQ(item->isLive(),
                  item->state() == RumbleChannelState::Connected);
        events.emplace_back("live");
    });
    std::ignore = item->reconnectAvailabilityChanged.connect([&] {
        events.emplace_back("reconnect");
    });
    int reconnects = 0;
    ASSERT_TRUE(item->setReconnectDelegate([&] {
        ++reconnects;
    }));
    ASSERT_EQ(events, std::vector<std::string>{"reconnect"});
    events.clear();
    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(events, (std::vector<std::string>{"state", "state", "live"}));
    EXPECT_TRUE(item->isLive());
    EXPECT_FALSE(item->canSendMessage());
    EXPECT_FALSE(item->isWritable());
    const auto stable = events.size();
    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(events.size(), stable);
    events.clear();
    item->close();
    EXPECT_EQ(events, (std::vector<std::string>{"state", "live", "reconnect"}));
    EXPECT_FALSE(item->isLive());
    EXPECT_FALSE(item->canReconnect());
    item->reconnect();
    EXPECT_EQ(reconnects, 0);
}
TEST(RumbleChannelState, ReentrantTransitionIsRejectedWithoutMissingSignals)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("reentrant-state"));
    std::vector<RumbleChannelState> states;
    std::vector<bool> liveValues;
    std::optional<RumbleStateTransitionError> reentrantError;
    std::ignore = item->stateChanged.connect(
        [&](RumbleChannelState, RumbleChannelState current) {
            states.emplace_back(current);
            if (current == RumbleChannelState::Connected)
            {
                auto result = item->transitionTo(RumbleChannelState::Offline);
                EXPECT_FALSE(result);
                if (!result)
                    reentrantError = result.error();
            }
        });
    std::ignore = item->liveStatusChanged.connect([&] {
        liveValues.emplace_back(item->isLive());
    });

    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(item->state(), RumbleChannelState::Connected);
    EXPECT_EQ(reentrantError, RumbleStateTransitionError::ReentrantTransition);
    EXPECT_EQ(states, (std::vector<RumbleChannelState>{
                          RumbleChannelState::Connecting,
                          RumbleChannelState::Connected,
                      }));
    EXPECT_EQ(liveValues, std::vector<bool>{true});

    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Offline));
    EXPECT_EQ(item->state(), RumbleChannelState::Offline);
    EXPECT_EQ(states.back(), RumbleChannelState::Offline);
    EXPECT_EQ(liveValues, (std::vector<bool>{true, false}));
}
TEST(RumbleChannelState, ReentrantVoidCloseRunsAfterOuterSignalPair)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("reentrant-close"));
    std::vector<bool> liveValues;
    std::ignore = item->stateChanged.connect(
        [&](RumbleChannelState, RumbleChannelState current) {
            if (current == RumbleChannelState::Connected)
                item->close();
        });
    std::ignore = item->liveStatusChanged.connect([&] {
        liveValues.emplace_back(item->isLive());
    });

    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(item->state(), RumbleChannelState::Closed);
    EXPECT_EQ(liveValues, (std::vector<bool>{true, false}));
    EXPECT_EQ(dispatcher->pending(), 0U);
}
TEST(RumbleChannelState, ReplacedReconnectTargetIsDestroyedAfterUnlock)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("reentrant-reconnect"));
    auto destructorRan = std::make_shared<bool>(false);
    auto callbackError = std::make_shared<std::optional<RumbleChannelError>>();
    auto probe = std::make_shared<DestructionCallbackProbe>([=] {
        *destructorRan = true;
        auto result = item->setReconnectDelegate([] {});
        if (!result)
            *callbackError = result.error();
    });
    ASSERT_TRUE(item->setReconnectDelegate([probe] {}));
    probe.reset();

    ASSERT_TRUE(item->setReconnectDelegate([] {}));
    EXPECT_TRUE(*destructorRan);
    EXPECT_FALSE(*callbackError);
    EXPECT_TRUE(item->canReconnect());
}
TEST(RumbleChannelState, RejectsPartialMutationAndStoresOnlyTypedFailure)
{
    static_assert(!std::is_constructible_v<RumbleFailure, RumbleFailureCategory,
                                           RumbleFailureCode, QString>);
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("failure"));
    int signals = 0;
    std::ignore = item->stateChanged.connect([&](auto, auto) {
        ++signals;
    });
    auto invalid = item->transitionTo(RumbleChannelState::Connected);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error(), RumbleStateTransitionError::InvalidTransition);
    auto missing = item->transitionTo(RumbleChannelState::Failed);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), RumbleStateTransitionError::MissingFailure);
    auto unexpected =
        item->transitionTo(RumbleChannelState::Offline, makeFailure());
    ASSERT_FALSE(unexpected);
    EXPECT_EQ(unexpected.error(),
              RumbleStateTransitionError::UnexpectedFailure);
    EXPECT_EQ(item->state(), RumbleChannelState::Unresolved);
    EXPECT_EQ(signals, 0);
    ASSERT_TRUE(item->transitionTo(RumbleChannelState::Failed, makeFailure()));
    ASSERT_TRUE(item->failure());
    EXPECT_EQ(item->failure()->category(), RumbleFailureCategory::Transport);
    EXPECT_EQ(item->failure()->code(), RumbleFailureCode::Timeout);
    auto safe = item->failure()->operatorSafeText();
    ASSERT_TRUE(safe);
    EXPECT_EQ(*safe, QStringLiteral("The request timed out."));
    EXPECT_EQ(signals, 1);
    dispatcher->ownerThread = false;
    auto wrong = item->transitionTo(RumbleChannelState::Unresolved);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error(), RumbleStateTransitionError::WrongThread);
    EXPECT_EQ(dispatcher->pending(), 0U);
    dispatcher->ownerThread = true;
}
TEST(RumbleChannelState, CommitsLifecycleStateAndMetadataAsOneSnapshot)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("lifecycle-snapshot"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);

    const RumbleLifecycleMetadata first{
        .consecutiveFailures = 1,
        .scheduledAtMs = 100,
        .deadlineAtMs = 500,
        .rateLimited = true,
        .retryCause = RumbleRetryCause::RateLimited,
    };
    int metadataSignals = 0;
    std::ignore = item->lifecycleMetadataChanged.connect([&] {
        ++metadataSignals;
        const auto snapshot = item->lifecycleSnapshot();
        EXPECT_EQ(snapshot.state, item->state());
        EXPECT_EQ(snapshot.metadata, item->lifecycleMetadata());
    });
    item->publishLifecycle(*token, RumbleChannelState::Backoff, first);
    EXPECT_EQ(item->lifecycleSnapshot(),
              (RumbleLifecycleSnapshot{RumbleChannelState::Backoff, first}));
    EXPECT_EQ(metadataSignals, 1);

    const RumbleLifecycleMetadata sameStateUpdate{
        .consecutiveFailures = 2,
        .scheduledAtMs = 500,
        .deadlineAtMs = 900,
        .retryCause = RumbleRetryCause::StreamEnded,
    };
    item->publishLifecycle(*token, RumbleChannelState::Backoff,
                           sameStateUpdate);
    EXPECT_EQ(item->lifecycleSnapshot(),
              (RumbleLifecycleSnapshot{RumbleChannelState::Backoff,
                                       sameStateUpdate}));
    EXPECT_EQ(metadataSignals, 2);

    const RumbleLifecycleMetadata rejected{
        .consecutiveFailures = 99,
        .retryCause = RumbleRetryCause::ProtocolFailure,
    };
    // Backoff -> Offline is not a state-machine edge. Neither half of the
    // copied snapshot may change when the publication is rejected.
    item->publishLifecycle(*token, RumbleChannelState::Offline, rejected);
    EXPECT_EQ(item->lifecycleSnapshot(),
              (RumbleLifecycleSnapshot{RumbleChannelState::Backoff,
                                       sameStateUpdate}));
    EXPECT_EQ(metadataSignals, 2);

    const RumbleLifecycleMetadata connecting{
        .consecutiveFailures = 1,
        .retryCause = RumbleRetryCause::StreamEnded,
    };
    const RumbleLifecycleMetadata reentrant{
        .consecutiveFailures = 2,
        .retryCause = RumbleRetryCause::TransportFailure,
    };
    std::ignore = item->stateChanged.connect(
        [&](RumbleChannelState, RumbleChannelState state) {
            if (state == RumbleChannelState::Connecting)
            {
                item->publishLifecycle(*token, RumbleChannelState::Failed,
                                       reentrant, makeFailure());
            }
        });
    item->publishLifecycle(*token, RumbleChannelState::Connecting, connecting);
    EXPECT_EQ(
        item->lifecycleSnapshot(),
        (RumbleLifecycleSnapshot{RumbleChannelState::Connecting, connecting}));
    EXPECT_FALSE(item->failure());

    item->close();
    EXPECT_EQ(item->lifecycleSnapshot(),
              (RumbleLifecycleSnapshot{RumbleChannelState::Closed, {}}));
}
TEST(RumbleChannelLifetime, OffThreadTypedPublicationWaitsForOwnerDrain)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("threaded"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto metadata =
        RumbleResolvedMetadata::create(QStringLiteral("Threaded Display"));
    ASSERT_TRUE(metadata);

    EXPECT_EQ(item->getDisplayName(), QStringLiteral("threaded"));
    EXPECT_EQ(item->getLocalizedName(), QStringLiteral("threaded"));
    EXPECT_NE(item->getName(), item->getLocalizedName());

    int displaySignals = 0;
    std::thread::id displaySignalThread;
    std::ignore = item->displayNameChanged.connect([&] {
        ++displaySignals;
        displaySignalThread = std::this_thread::get_id();
    });

    std::promise<void> publicationReturned;
    auto returned = publicationReturned.get_future();
    std::thread::id producerThread;
    std::thread producer([item, token = *token, metadata = std::move(*metadata),
                          &publicationReturned, &producerThread]() mutable {
        producerThread = std::this_thread::get_id();
        item->publishMetadata(token, std::move(metadata));
        publicationReturned.set_value();
    });
    returned.wait();
    producer.join();

    EXPECT_NE(producerThread, dispatcher->ownerThreadId);
    EXPECT_EQ(dispatcher->pending(), 1U);
    EXPECT_FALSE(item->metadata());
    EXPECT_EQ(item->getDisplayName(), QStringLiteral("threaded"));
    EXPECT_EQ(item->getLocalizedName(), QStringLiteral("threaded"));
    EXPECT_EQ(displaySignals, 0);

    dispatcher->runAll();
    ASSERT_TRUE(item->metadata());
    EXPECT_EQ(item->getDisplayName(), QStringLiteral("Threaded Display"));
    EXPECT_EQ(item->getLocalizedName(), QStringLiteral("Threaded Display"));
    EXPECT_EQ(displaySignals, 1);
    EXPECT_EQ(displaySignalThread, dispatcher->ownerThreadId);
}
TEST(RumbleChannelLifetime, TypedUpdatesRunOnceInQueuedFifoOrder)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("updates"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Display"),
        makeKey(RumbleChannelKeyKind::ChannelSlug, QStringLiteral("canonical")),
        makeKey(RumbleChannelKeyKind::EmbedId, QStringLiteral("v123")),
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("00042")));
    ASSERT_TRUE(metadata);
    auto b1 = makePublication(QStringLiteral("b1"));
    auto b2 = makePublication(QStringLiteral("b2"));
    auto normal = makePublication(QStringLiteral("normal"));
    auto rant = makePublication(QStringLiteral("rant"), true);
    auto pin = makePublication(QStringLiteral("pin"), true);
    auto b1Message = b1.message();
    auto b2Message = b2.message();
    auto normalMessage = normal.message();
    auto rantMessage = rant.message();
    std::vector<std::string> events;
    std::ignore = item->displayNameChanged.connect([&] {
        EXPECT_EQ(item->getDisplayName(), QStringLiteral("Display"));
        EXPECT_EQ(item->getLocalizedName(), QStringLiteral("Display"));
        events.emplace_back("display");
    });
    std::ignore = item->locatorChanged.connect([&] {
        EXPECT_EQ(item->getCurrentStreamID(), QStringLiteral("42"));
        events.emplace_back("locator");
    });
    std::ignore = item->filledInMessages.connect([&](const auto &messages) {
        EXPECT_EQ(messages.size(), 2U);
        events.emplace_back("bootstrap");
    });
    std::ignore = item->messageAppended.connect([&](auto &, auto) {
        events.emplace_back("realtime");
    });
    std::ignore = item->pinnedMessageChanged.connect([&] {
        events.emplace_back("pin");
    });
    dispatcher->ownerThread = false;
    item->publishMetadata(*token, *metadata);
    item->publishBootstrap(*token, {b1, b2});
    item->publishRealtime(*token, normal);
    item->publishRealtime(*token, rant);
    item->publishDeletion(*token, makeMessageId(QStringLiteral("b1")));
    item->publishNonRantClear(*token,
                              {.mode = RumbleNonRantClearMode::ListedIds,
                               .ids = {makeMessageId(QStringLiteral("b2")),
                                       makeMessageId(QStringLiteral("rant"))}});
    item->publishNonRantClear(
        *token, {.mode = RumbleNonRantClearMode::AllKnown, .ids = {}});
    item->publishPinnedMessage(*token, RumblePinnedMessage{pin});
    EXPECT_EQ(dispatcher->pending(), 8U);
    EXPECT_EQ(item->countMessages(), 0U);
    dispatcher->ownerThread = true;
    dispatcher->runAll();
    EXPECT_EQ(events,
              (std::vector<std::string>{"display", "locator", "bootstrap",
                                        "realtime", "realtime", "pin"}));
    const auto snapshot = item->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 4U);
    EXPECT_EQ(snapshot[0]->id, QStringLiteral("b1"));
    EXPECT_EQ(snapshot[1]->id, QStringLiteral("b2"));
    EXPECT_EQ(snapshot[2]->id, QStringLiteral("normal"));
    EXPECT_EQ(snapshot[3]->id, QStringLiteral("rant"));
    EXPECT_TRUE(b1Message->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(b2Message->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(normalMessage->flags.has(MessageFlag::Disabled));
    EXPECT_FALSE(rantMessage->flags.has(MessageFlag::Disabled));
    ASSERT_TRUE(item->pinnedMessage());
    EXPECT_EQ(item->pinnedMessage()->publication.id().value(),
              QStringLiteral("pin"));
}
TEST(RumbleChannelModeration, PublishesReadableSnapshotAndResetsStreamScope)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("moderation"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    int signals = 0;
    std::ignore = item->moderationStateChanged.connect([&] {
        ++signals;
    });

    item->publishModeration(
        *token, rumble::DeleteMessagesEvent{{QStringLiteral("message-1")}});
    auto snapshot = item->moderationSnapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_FALSE(snapshot->scope.accountId);
    EXPECT_EQ(snapshot->scope.channelId.value(), QStringLiteral("moderation"));
    EXPECT_TRUE(snapshot->deletedMessageIds.contains("message-1"));
    EXPECT_EQ(signals, 1);

    // A semantic replay is observable as no state change or signal.
    item->publishModeration(
        *token, rumble::DeleteMessagesEvent{{QStringLiteral("message-1")}});
    EXPECT_EQ(signals, 1);

    DeferredChannelAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession("SYNTHETIC_SESSION_CANARY"));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    item->setSessionController(session);
    item->publishModeration(
        *token, rumble::DeleteMessagesEvent{{QStringLiteral("account-reset")}});
    snapshot = item->moderationSnapshot();
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot->scope.accountId);
    EXPECT_EQ(snapshot->scope.accountId->value(),
              QStringLiteral("channel-test"));
    EXPECT_FALSE(snapshot->deletedMessageIds.contains("message-1"));
    EXPECT_TRUE(snapshot->deletedMessageIds.contains("account-reset"));
    EXPECT_EQ(signals, 2);

    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Display"), std::nullopt, std::nullopt,
        makeKey(RumbleChannelKeyKind::StreamId, QStringLiteral("00042")));
    ASSERT_TRUE(metadata);
    item->publishMetadata(*token, std::move(*metadata));
    item->publishModeration(
        *token, rumble::DeleteMessagesEvent{{QStringLiteral("message-2")}});
    snapshot = item->moderationSnapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->scope.channelId.value(), QStringLiteral("42"));
    EXPECT_FALSE(snapshot->deletedMessageIds.contains("message-1"));
    EXPECT_TRUE(snapshot->deletedMessageIds.contains("message-2"));
    EXPECT_EQ(signals, 3);
}
TEST(RumbleChannelHistory, ReconnectBootstrapFillsChronologicallyWithoutOverlap)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("history-overlap"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);

    item->publishRealtime(*token,
                          makeTimedPublication(QStringLiteral("live"), 20));
    item->publishBootstrap(
        *token, {
                    makeTimedPublication(QStringLiteral("older"), 10),
                    makeTimedPublication(QStringLiteral("live"), 20),
                    makeTimedPublication(QStringLiteral("newer"), 30),
                });

    const auto messages = item->getMessageSnapshot();
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_EQ(messages[0]->id, QStringLiteral("older"));
    EXPECT_EQ(messages[1]->id, QStringLiteral("live"));
    EXPECT_EQ(messages[2]->id, QStringLiteral("newer"));
}
TEST(RumbleChannelLifetime, AdaptsTypedMessageDtoWithoutLosingRant)
{
    MockApplication app;
    const auto timestamp =
        QDateTime::fromString(QStringLiteral("2026-01-01T00:00:00Z"),
                              Qt::ISODate)
            .toUTC();
    rumble::MessageDto dto{
        .id = QStringLiteral("typed-rant"),
        .userId = QStringLiteral("user"),
        .channelId = QStringLiteral("channel"),
        .text = QStringLiteral("message"),
        .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
        .timestamp = timestamp,
        .rant = true,
    };

    auto publication = RumbleMessagePublication::fromDto(dto);
    ASSERT_TRUE(publication);
    EXPECT_TRUE(publication->isRant());
    EXPECT_EQ(publication->id().value(), dto.id);
    EXPECT_EQ(publication->message()->platform, MessagePlatform::Rumble);
}
TEST(RumbleChannelLifetime, PublicationRejectsNonNormalizedMessageIds)
{
    for (const auto &id : QStringList{
             QStringLiteral(" leading"),
             QStringLiteral("trailing "),
             QStringLiteral("control\0id"),
             QStringLiteral("line\nid"),
         })
    {
        auto typed = RumbleMessageId::fromNormalized(id);
        ASSERT_FALSE(typed);
        EXPECT_EQ(typed.error(), RumbleMessageIdError::NotNormalized);

        auto message = std::make_shared<Message>();
        message->id = id;
        auto publication = RumbleMessagePublication::create(message, false);
        ASSERT_FALSE(publication);
        EXPECT_EQ(publication.error(), RumbleMessageIdError::NotNormalized);
    }
}
TEST(RumbleChannelLifetime, ReplacementCancelsAndStaleGenerationIsInert)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("generation"));
    auto firstCount = std::make_shared<int>(0);
    auto secondCount = std::make_shared<int>(0);
    auto first = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(first);
    ASSERT_TRUE(item->attachOperation(
        *first, std::make_unique<FakeOperation>(firstCount)));
    auto second = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(second);
    EXPECT_EQ(*firstCount, 1);
    ASSERT_TRUE(item->attachOperation(
        *second, std::make_unique<FakeOperation>(secondCount)));
    item->publishRealtime(*first, makePublication(QStringLiteral("stale")));
    EXPECT_EQ(item->countMessages(), 0U);
    item->publishRealtime(*second, makePublication(QStringLiteral("current")));
    EXPECT_EQ(item->countMessages(), 1U);
    item->close();
    EXPECT_EQ(*secondCount, 1);
    item->publishRealtime(*second, makePublication(QStringLiteral("late")));
    EXPECT_EQ(item->countMessages(), 1U);
    item->close();
    EXPECT_EQ(*secondCount, 1);
}
TEST(RumbleChannelLifetime, CancellationMayReenterWithoutHoldingSlotMutex)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("reentrant-cancel"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    bool callbackRan = false;
    std::optional<RumbleChannelError> callbackError;
    ASSERT_TRUE(
        item->attachOperation(*token, std::make_unique<CallbackOperation>([&] {
            callbackRan = true;
            auto result = item->completeOperation(*token);
            if (!result)
                callbackError = result.error();
        })));

    item->close();
    EXPECT_TRUE(callbackRan);
    EXPECT_EQ(callbackError, RumbleChannelError::StaleOperation);
    EXPECT_EQ(item->state(), RumbleChannelState::Closed);
}
TEST(RumbleChannelLifetime, RejectedOffOwnerHandleIsDestroyedOnOwner)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("off-owner-handle"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto cancelCount = std::make_shared<int>(0);
    auto cancelThread = std::make_shared<std::thread::id>();
    auto destructionCount = std::make_shared<int>(0);
    auto destructionThread = std::make_shared<std::thread::id>();
    std::optional<RumbleChannelError> error;
    std::thread::id producerThread;
    std::thread producer(
        [&, operation = std::make_unique<FakeOperation>(
                cancelCount, nullptr, cancelThread, destructionCount,
                destructionThread)]() mutable {
            producerThread = std::this_thread::get_id();
            auto result = item->attachOperation(*token, std::move(operation));
            if (!result)
                error = result.error();
        });
    producer.join();

    EXPECT_EQ(error, RumbleChannelError::WrongThread);
    EXPECT_EQ(*cancelCount, 1);
    EXPECT_EQ(*cancelThread, producerThread);
    EXPECT_EQ(*destructionCount, 0);
    EXPECT_EQ(dispatcher->pending(), 1U);
    dispatcher->runAll();
    EXPECT_EQ(*destructionCount, 1);
    EXPECT_EQ(*destructionThread, std::this_thread::get_id());
}
TEST(RumbleChannelLifetime, MisroutedDispatcherTaskCannotMutate)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("owner-check"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);

    dispatcher->ownerThread = false;
    item->publishRealtime(*token, makePublication(QStringLiteral("misrouted")));
    ASSERT_EQ(dispatcher->pending(), 1U);
    auto task = dispatcher->takeNext();
    ASSERT_TRUE(task);
    task();

    EXPECT_EQ(item->countMessages(), 0U);
    EXPECT_EQ(dispatcher->pending(), 0U);
    dispatcher->ownerThread = true;
}
TEST(RumbleChannelLifetime, CompletionReleasesWithoutCancellation)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("completion"));
    auto cancelCount = std::make_shared<int>(0);
    auto releaseCount = std::make_shared<int>(0);
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    ASSERT_TRUE(item->attachOperation(
        *token, std::make_unique<FakeOperation>(cancelCount, releaseCount)));
    ASSERT_TRUE(item->completeOperation(*token));
    EXPECT_EQ(*cancelCount, 0);
    EXPECT_EQ(*releaseCount, 1);
    auto repeated = item->completeOperation(*token);
    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error(), RumbleChannelError::StaleOperation);
    item->close();
    EXPECT_EQ(*cancelCount, 0);
    EXPECT_EQ(*releaseCount, 1);
}
TEST(RumbleChannelLifetime, CloseBeforeDispatchAndReconnectDiscardQueuedWork)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("close-before-dispatch"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    int reconnects = 0;
    ASSERT_TRUE(item->setReconnectDelegate([&] {
        ++reconnects;
    }));
    dispatcher->ownerThread = false;
    item->publishRealtime(*token, makePublication(QStringLiteral("queued")));
    item->reconnect();
    EXPECT_EQ(dispatcher->pending(), 2U);
    dispatcher->ownerThread = true;
    item->close();
    dispatcher->runAll();
    EXPECT_EQ(item->countMessages(), 0U);
    EXPECT_EQ(reconnects, 0);
}
TEST(RumbleChannelLifetime, OffOwnerCloseGatesEarlierAndLaterQueuedWork)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("off-owner-close"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    int reconnects = 0;
    std::vector<std::string> events;
    std::ignore = item->stateChanged.connect([&](auto, auto) {
        events.emplace_back("state");
    });
    std::ignore = item->reconnectAvailabilityChanged.connect([&] {
        events.emplace_back("reconnect");
    });
    ASSERT_TRUE(item->setReconnectDelegate([&] {
        ++reconnects;
    }));
    ASSERT_EQ(events, std::vector<std::string>{"reconnect"});
    events.clear();
    dispatcher->ownerThread = false;
    item->publishRealtime(*token,
                          makePublication(QStringLiteral("before-close")));
    item->reconnect();
    item->close();
    item->close();
    item->publishRealtime(*token,
                          makePublication(QStringLiteral("after-close")));
    item->reconnect();
    EXPECT_EQ(dispatcher->pending(), 3U);
    EXPECT_TRUE(item->canReconnect());
    EXPECT_TRUE(events.empty());
    dispatcher->ownerThread = true;
    dispatcher->runAll();
    EXPECT_EQ(item->state(), RumbleChannelState::Closed);
    EXPECT_EQ(item->countMessages(), 0U);
    EXPECT_EQ(reconnects, 0);
    EXPECT_EQ(events, (std::vector<std::string>{"state", "reconnect"}));
}
TEST(RumbleChannelLifetime, DestroyBeforeCallbackPublishesNothing)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    std::weak_ptr<RumbleChannel> weak;
    {
        auto item = makeChannel(provider, QStringLiteral("destroy"));
        auto token = item->beginOperation(RumbleOperationKind::Connection);
        ASSERT_TRUE(token);
        dispatcher->ownerThread = false;
        item->publishRealtime(*token,
                              makePublication(QStringLiteral("queued")));
        weak = item;
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(dispatcher->pending(), 2U);
    dispatcher->ownerThread = true;
    dispatcher->runAll();
}
TEST(RumbleChannelLifetime, DiscardedCleanupEventReleasesOnOwner)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("discarded-cleanup"));
    auto probeCount = std::make_shared<int>(0);
    auto probeThread = std::make_shared<std::thread::id>();
    auto probe =
        std::make_shared<DestructionThreadProbe>(probeCount, probeThread);
    ASSERT_TRUE(item->setReconnectDelegate([probe] {}));
    probe.reset();
    std::weak_ptr<RumbleChannel> weak = item;

    dispatcher->ownerThread = false;
    item.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(*probeCount, 0);
    EXPECT_EQ(dispatcher->pending(), 1U);
    dispatcher->ownerThread = true;
    dispatcher->discardAll();
    EXPECT_EQ(dispatcher->pending(), 0U);
    EXPECT_EQ(*probeCount, 1);
    EXPECT_EQ(*probeThread, std::this_thread::get_id());
}
TEST(RumbleChannelLifetime, ProducerCannotBecomeChannelDestructionThread)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("owner-destruction"));
    auto cancelCount = std::make_shared<int>(0);
    auto cancelThread = std::make_shared<std::thread::id>();
    auto destructionCount = std::make_shared<int>(0);
    auto destructionThread = std::make_shared<std::thread::id>();
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    ASSERT_TRUE(item->attachOperation(
        *token,
        std::make_unique<FakeOperation>(cancelCount, nullptr, cancelThread,
                                        destructionCount, destructionThread)));
    std::weak_ptr<RumbleChannel> weak = item;

    std::promise<void> acquired;
    auto acquiredFuture = acquired.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::atomic_bool producerLocked{false};
    std::thread::id producerThread;
    std::thread producer(
        [weak, &acquired, releaseFuture, &producerLocked, &producerThread] {
            producerThread = std::this_thread::get_id();
            auto last = weak.lock();
            producerLocked.store(static_cast<bool>(last),
                                 std::memory_order_release);
            acquired.set_value();
            releaseFuture.wait();
            last.reset();
        });
    acquiredFuture.wait();
    EXPECT_TRUE(producerLocked.load(std::memory_order_acquire));
    item.reset();
    // Ordinary owner work is now rejected. Owner-affine disposal is a
    // separate guaranteed path and must still retain the raw channel.
    dispatcher->acceptTasks = false;
    release.set_value();
    producer.join();

    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(*cancelCount, 1);
    EXPECT_EQ(*cancelThread, producerThread);
    EXPECT_EQ(*destructionCount, 0);
    EXPECT_EQ(dispatcher->disposalCount, 0);
    EXPECT_EQ(dispatcher->pending(), 2U);
    dispatcher->acceptTasks = true;
    dispatcher->runAll();
    EXPECT_EQ(*cancelCount, 1);
    EXPECT_EQ(*destructionCount, 1);
    EXPECT_EQ(*destructionThread, std::this_thread::get_id());
    EXPECT_EQ(dispatcher->disposalCount, 2);
    EXPECT_EQ(dispatcher->disposalThread, std::this_thread::get_id());
}
TEST(RumbleChannelLifetime, QtOwnerCleanupSurvivesExternalContextDestruction)
{
    MockApplication app;
    auto owner = std::make_unique<QObject>();
    auto dispatcher = makeQtRumbleDispatcher(owner.get());
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("qt-owner-cleanup"));
    auto probeCount = std::make_shared<int>(0);
    auto probeThread = std::make_shared<std::thread::id>();
    auto probe =
        std::make_shared<DestructionThreadProbe>(probeCount, probeThread);
    ASSERT_TRUE(item->setReconnectDelegate([probe] {}));
    probe.reset();
    std::weak_ptr<RumbleChannel> weak = item;

    std::promise<void> acquired;
    auto acquiredFuture = acquired.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::thread producer([weak, &acquired, releaseFuture] {
        auto last = weak.lock();
        acquired.set_value();
        releaseFuture.wait();
        last.reset();
    });
    acquiredFuture.wait();
    item.reset();
    release.set_value();
    producer.join();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(*probeCount, 0);

    owner.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(*probeCount, 1);
    EXPECT_EQ(*probeThread, std::this_thread::get_id());
}
TEST(RumbleChannelLifetime, GenuineLateWeakCallbackCannotPublish)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("late-callback"));
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    auto publication = makePublication(QStringLiteral("too-late"));
    std::weak_ptr<RumbleChannel> weak = item;
    std::promise<void> invoke;
    auto invokeFuture = invoke.get_future().share();
    std::atomic_bool callbackLocked{false};
    std::thread producer([weak, token = *token,
                          publication = std::move(publication), invokeFuture,
                          &callbackLocked]() mutable {
        invokeFuture.wait();
        if (auto channel = weak.lock())
        {
            callbackLocked.store(true, std::memory_order_release);
            channel->publishRealtime(token, std::move(publication));
        }
    });
    item.reset();
    EXPECT_TRUE(weak.expired());
    invoke.set_value();
    producer.join();

    EXPECT_FALSE(callbackLocked.load(std::memory_order_acquire));
    EXPECT_EQ(dispatcher->pending(), 0U);
}
TEST(RumbleChannelLifetime, RejectedCloseDispatchStillCancelsAndTerminates)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("rejected-close"));
    auto cancelCount = std::make_shared<int>(0);
    auto token = item->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);
    ASSERT_TRUE(item->attachOperation(
        *token, std::make_unique<FakeOperation>(cancelCount)));
    int reconnectSignals = 0;
    auto probeCount = std::make_shared<int>(0);
    auto probeThread = std::make_shared<std::thread::id>();
    auto probe =
        std::make_shared<DestructionThreadProbe>(probeCount, probeThread);
    ASSERT_TRUE(item->setReconnectDelegate([probe] {}));
    probe.reset();
    std::ignore = item->reconnectAvailabilityChanged.connect([&] {
        ++reconnectSignals;
    });

    dispatcher->ownerThread = false;
    dispatcher->acceptTasks = false;
    item->close();
    EXPECT_EQ(*cancelCount, 1);
    EXPECT_EQ(item->state(), RumbleChannelState::Closed);
    EXPECT_FALSE(item->canReconnect());
    EXPECT_EQ(reconnectSignals, 0);
    EXPECT_EQ(*probeCount, 0);
    EXPECT_EQ(dispatcher->pending(), 2U);
    item->close();
    EXPECT_EQ(*cancelCount, 1);
    dispatcher->acceptTasks = true;
    dispatcher->ownerThread = true;
    dispatcher->runAll();
    EXPECT_EQ(*probeCount, 1);
    EXPECT_EQ(*probeThread, std::this_thread::get_id());
    EXPECT_EQ(dispatcher->disposalCount, 2);
    item.reset();
    EXPECT_EQ(*probeCount, 1);
}
TEST(RumbleChannelLifetime, RejectedCloseReleasesSelfCapturingDelegateOnOwner)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("self-cycle-close"));
    std::weak_ptr<RumbleChannel> weak = item;
    ASSERT_TRUE(item->setReconnectDelegate([self = item] {
        (void)self;
    }));

    dispatcher->ownerThread = false;
    dispatcher->acceptTasks = false;
    item->close();
    item.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(dispatcher->pending(), 1U);

    dispatcher->acceptTasks = true;
    dispatcher->ownerThread = true;
    dispatcher->runAll();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(dispatcher->disposalThread, std::this_thread::get_id());
}
TEST(RumbleChannelLifetime, RejectedProviderShutdownLeavesNoLiveOperation)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("rejected-shutdown"));
    ASSERT_TRUE(provider.associateAlias(item, RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("vshutdown")));
    auto cancelCount = std::make_shared<int>(0);
    auto token = item->beginOperation(RumbleOperationKind::Resolver);
    ASSERT_TRUE(token);
    ASSERT_TRUE(item->attachOperation(
        *token, std::make_unique<FakeOperation>(cancelCount)));
    std::weak_ptr<RumbleChannel> weak = item;

    dispatcher->ownerThread = false;
    dispatcher->acceptTasks = false;
    provider.shutdown();
    EXPECT_EQ(*cancelCount, 1);
    EXPECT_EQ(item->state(), RumbleChannelState::Closed);
    EXPECT_EQ(dispatcher->pending(), 1U);
    dispatcher->acceptTasks = true;
    dispatcher->ownerThread = true;
    dispatcher->runAll();
    auto lookup = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                       QStringLiteral("vshutdown"));
    ASSERT_FALSE(lookup);
    EXPECT_EQ(lookup.error().code, RumbleProviderErrorCode::Shutdown);
    item.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(*cancelCount, 1);
}
TEST(RumbleChannelLifetime, ProviderShutdownClosesEachAliasedChannelOnce)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("shutdown"));
    std::weak_ptr<RumbleChannel> weak = item;
    ASSERT_TRUE(provider.associateAlias(item, RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("v999")));
    auto count = std::make_shared<int>(0);
    auto token = item->beginOperation(RumbleOperationKind::Resolver);
    ASSERT_TRUE(token);
    ASSERT_TRUE(
        item->attachOperation(*token, std::make_unique<FakeOperation>(count)));
    provider.shutdown();
    EXPECT_EQ(item->state(), RumbleChannelState::Closed);
    EXPECT_EQ(*count, 1);
    provider.shutdown();
    EXPECT_EQ(*count, 1);
    item.reset();
    EXPECT_EQ(*count, 1);
    EXPECT_TRUE(weak.expired());
}
TEST(RumbleChannelLifetime, ConstructionStartsNoWorkTimerOrPublication)
{
    MockApplication app;
    auto dispatcher = std::make_shared<FifoDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto item = makeChannel(provider, QStringLiteral("side-effect-free"));
    int signals = 0;
    std::ignore = item->stateChanged.connect([&](auto, auto) {
        ++signals;
    });
    EXPECT_EQ(item->state(), RumbleChannelState::Unresolved);
    EXPECT_EQ(item->countMessages(), 0U);
    EXPECT_EQ(dispatcher->pending(), 0U);
    EXPECT_EQ(signals, 0);
    EXPECT_TRUE(item->isRumbleChannel());
    EXPECT_EQ(item->messagePlatform(), MessagePlatform::Rumble);
    EXPECT_FALSE(item->canSendMessage());
    EXPECT_FALSE(item->isWritable());
    EXPECT_FALSE(item->canReconnect());
}
}  // namespace chatterino
