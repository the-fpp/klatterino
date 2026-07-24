// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"

#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "util/MultiChannel.hpp"

#include <QHash>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace chatterino {
namespace {

class RoutingChannel final : public Channel
{
public:
    explicit RoutingChannel(QString name)
        : Channel(name, Channel::Type::Twitch)
    {
        this->context_.platform = QStringLiteral("twitch");
        this->context_.channelID = std::move(name);
    }

    bool canSendMessage() const override
    {
        return this->context_.authenticated;
    }

    bool isWritable() const override
    {
        return this->context_.writable;
    }

    MessageSendContext messageSendContext() const override
    {
        return this->context_;
    }

    void sendMessage(const QString &message) override
    {
        this->sent.push_back(message);
        if (this->onSend)
        {
            this->onSend();
        }
    }

    void sendMessageAsync(QString message, SendCallback callback) override
    {
        this->sent.push_back(std::move(message));
        if (this->deferAsync)
            this->pendingSend = std::move(callback);
        else if (callback)
            callback({SendOutcome::Confirmed, {}});
    }

    MessageSendContext context_;
    std::vector<QString> sent;
    std::function<void()> onSend;
    bool deferAsync = false;
    SendCallback pendingSend;
};

class LegacySendOnlyChannel final : public Channel
{
public:
    LegacySendOnlyChannel()
        : Channel(QStringLiteral("legacy"), Channel::Type::Twitch)
    {
    }

    bool canSendMessage() const override { return true; }
    bool isWritable() const override { return true; }
    MessageSendContext messageSendContext() const override
    {
        return {.platform = QStringLiteral("twitch"),
                .channelID = QStringLiteral("legacy"),
                .writable = true,
                .authenticated = true};
    }
    void sendMessage(const QString &message) override
    {
        ++dispatches;
        sent = message;
    }

    int dispatches = 0;
    QString sent;
};

class RoutingTwitchIrcServer : public mock::MockTwitchIrcServer
{
public:
    ChannelPtr getOrAddChannel(const QString &name) override
    {
        auto &entry = this->channels[name];
        if (!entry)
        {
            entry = std::make_shared<RoutingChannel>(name);
        }
        return entry;
    }

    std::shared_ptr<RoutingChannel> get(const QString &name) const
    {
        return this->channels.value(name);
    }

    QHash<QString, std::shared_ptr<RoutingChannel>> channels;
};

class RoutingApplication : public mock::BaseApplication
{
public:
    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    RoutingTwitchIrcServer twitch;
};

MessagePtr ingress(QString id, QString login,
                   MessageFlags flags = MessageFlags{})
{
    auto message = std::make_shared<Message>();
    message->id = std::move(id);
    message->loginName = std::move(login);
    message->flags = flags;
    return message;
}

class MultiChannelDispatchTest : public ::testing::Test
{
protected:
    std::shared_ptr<MultiChannel> makeMulti()
    {
        const MultiChannel::Spec specs[]{
            MultiChannel::Spec{MultiChannel::Platform::Twitch,
                               QStringLiteral("alpha")},
            MultiChannel::Spec{MultiChannel::Platform::Twitch,
                               QStringLiteral("beta")},
            MultiChannel::Spec{MultiChannel::Platform::Twitch,
                               QStringLiteral("gamma")}
        };
        return std::make_shared<MultiChannel>(
            std::span{specs},
            MultiChannelIndicatorMode::PlatformBadgeIfUnselected);
    }

    std::shared_ptr<RoutingChannel> channel(QString name) const
    {
        return this->application.twitch.get(name);
    }

    static void setSendable(const std::shared_ptr<RoutingChannel> &channel,
                            bool sendable)
    {
        channel->context_.writable = sendable;
        channel->context_.authenticated = sendable;
    }

    static void append(const std::shared_ptr<RoutingChannel> &channel,
                       const MessagePtr &message,
                       std::optional<MessageFlags> overridingFlags =
                           std::nullopt)
    {
        auto copy = message;
        channel->messageAppended.invoke(copy, std::move(overridingFlags));
    }

    RoutingApplication application;
};

TEST_F(MultiChannelDispatchTest, CoarseAvailabilityUsesEveryChild)
{
    auto multi = this->makeMulti();
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("beta")), true);

    EXPECT_TRUE(multi->canSendMessage());
    EXPECT_TRUE(multi->isWritable());

    setSendable(this->channel(QStringLiteral("beta")), false);
    EXPECT_FALSE(multi->canSendMessage());
    EXPECT_FALSE(multi->isWritable());
}

TEST_F(MultiChannelDispatchTest, CompatiblePrimaryWinsAndDispatchesOnce)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    append(beta, ingress(QStringLiteral("beta-live"), QStringLiteral("user")));

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("hello")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 0);
    EXPECT_FALSE(result.usedFallback);
    EXPECT_THAT(alpha->sent, ::testing::ElementsAre(QStringLiteral("hello")));
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelDispatchTest,
       AsyncOutcomeReturnsChosenDestinationAndNeverRetriesFallback)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->deferAsync = true;

    std::optional<Channel::SendResult> sendResult;
    std::optional<MultiChannel::DraftDispatchResult> route;
    multi->sendMessageDraftAsync(
        MessageDraft::fromPlainText(QStringLiteral("hello")),
        QStringLiteral("hello"), 0,
        MultiChannelRoutePolicy::CompatibleFallback,
        [&](MultiChannel::DraftDispatchResult selected,
            Channel::SendResult outcome) {
            route = std::move(selected);
            sendResult = std::move(outcome);
        });

    ASSERT_TRUE(alpha->pendingSend);
    EXPECT_TRUE(beta->sent.empty());
    alpha->pendingSend({Channel::SendOutcome::Ambiguous,
                        QStringLiteral("may have sent")});
    ASSERT_TRUE(route);
    EXPECT_EQ(route->destinationIndex, 0);
    ASSERT_TRUE(sendResult);
    EXPECT_EQ(sendResult->outcome, Channel::SendOutcome::Ambiguous);
    EXPECT_TRUE(beta->sent.empty());
}

TEST(MultiChannelDispatch,
     AsyncRoutingAdaptsLegacySendOnlyChildWithSingleDispatch)
{
    const MultiChannel::Spec spec{MultiChannel::Platform::Twitch,
                                  QStringLiteral("legacy")};
    auto legacy = std::make_shared<LegacySendOnlyChannel>();
    auto multi = std::make_shared<MultiChannel>(
        std::span<const MultiChannel::Spec>(&spec, 1),
        MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [legacy](const MultiChannel::Spec &) -> ChannelPtr { return legacy; });

    int callbacks = 0;
    std::optional<Channel::SendResult> result;
    multi->sendMessageDraftAsync(
        MessageDraft::fromPlainText(QStringLiteral("hello")),
        QStringLiteral("hello"), 0,
        MultiChannelRoutePolicy::CompatibleFallback,
        [&](MultiChannel::DraftDispatchResult route,
            Channel::SendResult outcome) {
            EXPECT_EQ(route.destinationIndex, 0);
            ++callbacks;
            result = std::move(outcome);
        });

    EXPECT_EQ(legacy->dispatches, 1);
    EXPECT_EQ(legacy->sent, QStringLiteral("hello"));
    EXPECT_EQ(callbacks, 1);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Channel::SendOutcome::Confirmed);
}

TEST_F(MultiChannelDispatchTest,
       RoutingDraftCanDifferFromTransformedProviderPayload)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    const auto draft =
        MessageDraft::fromPlainText(QStringLiteral(":shortcode:"));

    const auto result = multi->sendMessageDraft(
        draft, QStringLiteral("\U0001f600"), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_THAT(alpha->sent,
                ::testing::ElementsAre(QStringLiteral("\U0001f600")));
}

TEST_F(MultiChannelDispatchTest, PayloadLengthUsesTransformedText)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    alpha->context_.maxMessageLength = 3;
    const auto draft = MessageDraft::fromPlainText(QStringLiteral("ok"));

    const auto result = multi->sendMessageDraft(
        draft, QStringLiteral("too long"), 0);

    EXPECT_FALSE(result.sent());
    EXPECT_TRUE(alpha->sent.empty());
    ASSERT_FALSE(result.evaluations.empty());
    ASSERT_FALSE(result.evaluations[0].rejections.empty());
    EXPECT_EQ(result.evaluations[0].rejections[0].code,
              MessageDraftRejectionCode::TooLong);
}

TEST_F(MultiChannelDispatchTest, FallbackUsesNewestUniqueIngressActivity)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);

    append(beta, ingress(QStringLiteral("beta-1"), QStringLiteral("one")));
    append(gamma, ingress(QStringLiteral("gamma-1"), QStringLiteral("two")));
    append(beta, ingress(QStringLiteral("beta-2"), QStringLiteral("three")));
    // A reconnect replay of the same provider message must not reorder beta.
    append(gamma, ingress(QStringLiteral("gamma-1"), QStringLiteral("two")));

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("fallback")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1);
    EXPECT_TRUE(result.usedFallback);
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("fallback")));
    EXPECT_TRUE(gamma->sent.empty());
}

TEST_F(MultiChannelDispatchTest, CompleteDraftCapabilityCanRejectPrimary)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context_.emoteCapabilitiesComplete = true;
    beta->context_.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("stable")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {.platform = QStringLiteral("twitch")},
    };
    beta->context_.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });
    const auto draft = MessageDraft::reconstruct(
        QStringLiteral("OnlyBeta"), std::span{&selected, 1});

    const auto result = multi->sendMessageDraft(draft, 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("OnlyBeta")));
}

TEST_F(MultiChannelDispatchTest, HistorySystemAndLocalEchoDoNotWinActivity)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);

    append(beta, ingress(QStringLiteral("live"), QStringLiteral("viewer")));
    append(gamma, ingress(QStringLiteral("history"),
                          QStringLiteral("viewer"),
                          MessageFlags{MessageFlag::RecentMessage}));
    append(gamma, ingress(QStringLiteral("system"),
                          QStringLiteral("server"),
                          MessageFlags{MessageFlag::System}));
    append(gamma, ingress(QStringLiteral("fake"),
                          QStringLiteral("debug-user"),
                          MessageFlags{MessageFlag::Debug}));
    append(gamma, ingress(QStringLiteral("whisper"),
                          QStringLiteral("whisper-user"),
                          MessageFlags{MessageFlag::Whisper}));
    append(gamma, ingress({}, QStringLiteral("local-user")));
    append(gamma, ingress(QStringLiteral("no-login"), {}));

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("activity")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1);
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("activity")));
    EXPECT_TRUE(gamma->sent.empty());
}

TEST_F(MultiChannelDispatchTest,
       EffectiveOverrideFlagsAndInlineWhispersDoNotWinActivity)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);

    append(beta, ingress(QStringLiteral("live"), QStringLiteral("viewer")));

    const MessageFlag excludedOverrides[]{
        MessageFlag::System,
        MessageFlag::RecentMessage,
        MessageFlag::Debug,
        MessageFlag::DoNotLog,
        MessageFlag::Whisper
    };
    size_t index = 0;
    for (const auto flag : excludedOverrides)
    {
        append(gamma,
               ingress(QStringLiteral("override-%1").arg(
                           static_cast<qulonglong>(index++)),
                       QStringLiteral("repost-user")),
               MessageFlags{flag});
    }

    auto inlineWhisper = ingress(QStringLiteral("inline-whisper"),
                                 QStringLiteral("whisper-user"),
                                 MessageFlags{MessageFlag::Whisper});
    auto inlineWhisperFlags = inlineWhisper->flags;
    inlineWhisperFlags.set(MessageFlag::DoNotLog);
    append(gamma, inlineWhisper, inlineWhisperFlags);

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("effective flags")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1);
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("effective flags")));
    EXPECT_TRUE(gamma->sent.empty());
}

TEST(MultiChannelLiveMessageIdDeduplicator,
     UsesBoundedFifoWithoutRefreshingDuplicateOrder)
{
    static_assert(
        MultiChannel::LiveMessageIdDeduplicator::DEFAULT_CAPACITY == 50'000);
    MultiChannel::LiveMessageIdDeduplicator ids(2);

    EXPECT_TRUE(ids.remember(QStringLiteral("oldest")));
    EXPECT_TRUE(ids.remember(QStringLiteral("newer")));
    EXPECT_EQ(ids.size(), 2U);

    // A reconnect replay inside the retained window is ignored and does not
    // refresh the oldest ID's FIFO position.
    EXPECT_FALSE(ids.remember(QStringLiteral("oldest")));
    EXPECT_TRUE(ids.remember(QStringLiteral("newest")));
    EXPECT_EQ(ids.size(), 2U);
    EXPECT_FALSE(ids.contains(QStringLiteral("oldest")));
    EXPECT_TRUE(ids.contains(QStringLiteral("newer")));
    EXPECT_TRUE(ids.contains(QStringLiteral("newest")));

    // Once deterministically evicted, the same provider ID is a new
    // observation again; inserting it evicts the next FIFO entry.
    EXPECT_TRUE(ids.remember(QStringLiteral("oldest")));
    EXPECT_EQ(ids.size(), 2U);
    EXPECT_FALSE(ids.contains(QStringLiteral("newer")));
    EXPECT_TRUE(ids.contains(QStringLiteral("newest")));
    EXPECT_TRUE(ids.contains(QStringLiteral("oldest")));
}

TEST_F(MultiChannelDispatchTest, StableChildOrderBreaksNoActivityTie)
{
    auto multi = this->makeMulti();
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("beta")), true);
    setSendable(this->channel(QStringLiteral("gamma")), true);

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("tie")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1);
}

TEST_F(MultiChannelDispatchTest, PrimaryOnlyNeverFallsBack)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("/unknown")), 0,
        MultiChannelRoutePolicy::PrimaryOnly);

    EXPECT_FALSE(result.sent());
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
    ASSERT_EQ(result.evaluations.size(), 3U);
    EXPECT_FALSE(result.evaluations[0].sendable);
    EXPECT_TRUE(result.evaluations[1].sendable);
}

TEST_F(MultiChannelDispatchTest, SubmitTimeStateIsFreshAndNoRetryOccurs)
{
    auto multi = this->makeMulti();
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);
    append(gamma, ingress(QStringLiteral("recent"), QStringLiteral("viewer")));

    // This state change happens after completion and immediately before submit.
    setSendable(gamma, false);
    beta->onSend = [gamma] {
        // Even a re-entrant state change after the selected send cannot cause a
        // second destination attempt.
        gamma->context_.writable = true;
        gamma->context_.authenticated = true;
    };

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("fresh")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1);
    EXPECT_EQ(beta->sent.size(), 1U);
    EXPECT_TRUE(gamma->sent.empty());
}

TEST_F(MultiChannelDispatchTest, NoDestinationReturnsStructuredRejections)
{
    auto multi = this->makeMulti();
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("beta")), false);
    setSendable(this->channel(QStringLiteral("gamma")), false);

    const auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("blocked")), 0);

    EXPECT_FALSE(result.sent());
    ASSERT_EQ(result.evaluations.size(), 3U);
    for (const auto &evaluation : result.evaluations)
    {
        EXPECT_FALSE(evaluation.sendable);
        EXPECT_FALSE(evaluation.rejections.empty());
    }
}

}  // namespace
}  // namespace chatterino
