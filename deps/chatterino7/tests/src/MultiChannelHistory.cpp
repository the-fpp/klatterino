// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"

#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Channel.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/history/MessageHistoryLoadRegistry.hpp"
#include "util/MultiChannel.hpp"

#include <QDateTime>

#include <array>
#include <memory>
#include <vector>

using namespace chatterino;

namespace {

class TestTwitchIrcServer : public mock::MockTwitchIrcServer
{
public:
    ChannelPtr getOrAddChannel(const QString &channelName) override
    {
        const auto it = this->mockChannels.find(channelName);
        if (it != this->mockChannels.end())
        {
            if (auto existing = it->second.lock())
            {
                return existing;
            }
        }

        auto channel = std::make_shared<mock::MockChannel>(channelName);
        this->mockChannels[channelName] = channel;
        return channel;
    }
};

class TestApplication : public mock::BaseApplication
{
public:
    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    TestTwitchIrcServer twitch;
};

MessagePtr makeMessage(QString text, qint64 timestamp, bool recent)
{
    auto message = std::make_shared<Message>();
    message->messageText = std::move(text);
    message->serverReceivedTime =
        QDateTime::fromSecsSinceEpoch(timestamp, QTimeZone::UTC);
    if (recent)
    {
        message->flags.set(MessageFlag::RecentMessage);
    }
    return message;
}

MessagePtr makeHistory(QString text, qint64 timestamp)
{
    return makeMessage(std::move(text), timestamp, true);
}

MessagePtr makeLive(QString text, qint64 timestamp)
{
    return makeMessage(std::move(text), timestamp, false);
}

std::vector<QString> messageTexts(const Channel &channel)
{
    std::vector<QString> texts;
    for (const auto &message : channel.getMessageSnapshot())
    {
        texts.push_back(message->messageText);
    }
    return texts;
}

class MultiChannelHistoryTest : public ::testing::Test
{
protected:
    std::shared_ptr<MultiChannel> makeMultiChannel()
    {
        const std::array specs{
            MultiChannel::Spec{
                .platform = MultiChannel::Platform::Twitch,
                .name = QStringLiteral("alpha"),
            },
            MultiChannel::Spec{
                .platform = MultiChannel::Platform::Twitch,
                .name = QStringLiteral("beta"),
            },
        };

        return std::make_shared<MultiChannel>(
            specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected);
    }

    static ChannelPtr child(const std::shared_ptr<MultiChannel> &multi,
                            size_t index)
    {
        return multi->channels()[index].channel;
    }

    static void beginLoading(const std::shared_ptr<MultiChannel> &multi)
    {
        for (const auto &entry : multi->channels())
        {
            messagehistory::Registry::instance().setLoading(entry.channel);
        }
    }

    TestApplication application;
};

TEST_F(MultiChannelHistoryTest, CommitsOnlyAfterEveryChildLoads)
{
    auto multi = this->makeMultiChannel();
    auto alpha = child(multi, 0);
    auto beta = child(multi, 1);
    beginLoading(multi);

    alpha->addMessagesAtStart({makeHistory(QStringLiteral("alpha history"), 10)});
    messagehistory::Registry::instance().setLoaded(alpha);

    EXPECT_FALSE(multi->initialHistorySettled());
    EXPECT_TRUE(multi->getMessageSnapshot().empty());

    beta->addMessagesAtStart({makeHistory(QStringLiteral("beta history"), 20)});
    EXPECT_TRUE(multi->getMessageSnapshot().empty());

    messagehistory::Registry::instance().setLoaded(beta);

    EXPECT_TRUE(multi->initialHistorySettled());
    EXPECT_THAT(messageTexts(*multi),
                ::testing::ElementsAre(QStringLiteral("alpha history"),
                                       QStringLiteral("beta history")));
}

TEST_F(MultiChannelHistoryTest, EmptySuccessfulHistoryStillCommitsTransaction)
{
    auto multi = this->makeMultiChannel();
    auto alpha = child(multi, 0);
    auto beta = child(multi, 1);
    beginLoading(multi);

    alpha->addMessagesAtStart({makeHistory(QStringLiteral("only history"), 10)});
    messagehistory::Registry::instance().setLoaded(alpha);

    EXPECT_TRUE(multi->getMessageSnapshot().empty());

    // A provider reports success without invoking messagesAddedAtStart when it
    // has no messages. That must still complete the all-or-nothing load.
    messagehistory::Registry::instance().setLoaded(beta);

    EXPECT_TRUE(multi->initialHistorySettled());
    EXPECT_THAT(messageTexts(*multi),
                ::testing::ElementsAre(QStringLiteral("only history")));
}

TEST_F(MultiChannelHistoryTest,
       InterleavesDelayedChildBuffersChronologicallyAndStably)
{
    auto multi = this->makeMultiChannel();
    auto alpha = child(multi, 0);
    auto beta = child(multi, 1);
    beginLoading(multi);

    alpha->addMessagesAtStart(
        {makeHistory(QStringLiteral("alpha late"), 30),
         makeHistory(QStringLiteral("alpha tie first"), 20),
         makeHistory(QStringLiteral("alpha tie second"), 20),
         makeHistory(QStringLiteral("alpha early"), 10)});
    beta->addMessagesAtStart({makeHistory(QStringLiteral("beta latest"), 40),
                              makeHistory(QStringLiteral("beta tie"), 20),
                              makeHistory(QStringLiteral("beta early"), 15)});

    // Complete the second child first to ensure callback order cannot group or
    // reorder the aggregate history.
    messagehistory::Registry::instance().setLoaded(beta);
    EXPECT_TRUE(multi->getMessageSnapshot().empty());

    messagehistory::Registry::instance().setLoaded(alpha);

    EXPECT_TRUE(multi->initialHistorySettled());
    EXPECT_THAT(
        messageTexts(*multi),
        ::testing::ElementsAre(
            QStringLiteral("alpha early"), QStringLiteral("beta early"),
            QStringLiteral("alpha tie first"),
            QStringLiteral("alpha tie second"), QStringLiteral("beta tie"),
            QStringLiteral("alpha late"), QStringLiteral("beta latest")));
}

TEST_F(MultiChannelHistoryTest, FailureDiscardsBufferedAndInflightHistory)
{
    auto multi = this->makeMultiChannel();
    auto alpha = child(multi, 0);
    auto beta = child(multi, 1);
    beginLoading(multi);

    QString failureMessage;
    auto failureConnection = multi->messageHistoryLoadFailed.connect(
        [&failureMessage](const QString &message) { failureMessage = message; });

    alpha->addMessagesAtStart(
        {makeHistory(QStringLiteral("buffered before failure"), 10)});
    messagehistory::Registry::instance().setFailed(
        beta, QStringLiteral("HTTP 500"));

    EXPECT_FALSE(multi->initialHistorySettled());
    EXPECT_TRUE(multi->getMessageSnapshot().empty());
    EXPECT_TRUE(failureMessage.contains(QStringLiteral("Twitch channel beta")));
    EXPECT_TRUE(failureMessage.contains(QStringLiteral("3 attempts")));
    EXPECT_TRUE(failureMessage.contains(QStringLiteral("HTTP 500")));

    // This simulates another provider callback arriving after the transaction
    // has already failed. Its history must also be discarded.
    alpha->addMessagesAtStart(
        {makeHistory(QStringLiteral("in flight after failure"), 20)});
    EXPECT_TRUE(multi->getMessageSnapshot().empty());

    messagehistory::Registry::instance().setLoaded(alpha);

    EXPECT_TRUE(multi->initialHistorySettled());
    EXPECT_TRUE(multi->getMessageSnapshot().empty());

    // Once all initial requests have reached a terminal state, later reconnect
    // history must no longer be suppressed by the aborted transaction.
    alpha->fillInMissingMessages(
        {makeHistory(QStringLiteral("later reconnect history"), 30)});
    EXPECT_THAT(messageTexts(*multi),
                ::testing::ElementsAre(
                    QStringLiteral("later reconnect history")));
}

TEST_F(MultiChannelHistoryTest, LiveMessagesRemainVisibleWhileHistoryIsBuffered)
{
    auto multi = this->makeMultiChannel();
    auto alpha = child(multi, 0);
    auto beta = child(multi, 1);
    beginLoading(multi);

    auto live = makeLive(QStringLiteral("live message"), 20);
    alpha->messageAppended.invoke(live, std::nullopt);
    alpha->addMessagesAtStart({makeHistory(QStringLiteral("alpha history"), 10)});
    beta->addMessagesAtStart({makeHistory(QStringLiteral("beta history"), 30)});

    EXPECT_THAT(messageTexts(*multi),
                ::testing::ElementsAre(QStringLiteral("live message")));

    messagehistory::Registry::instance().setLoaded(alpha);
    EXPECT_THAT(messageTexts(*multi),
                ::testing::ElementsAre(QStringLiteral("live message")));

    messagehistory::Registry::instance().setLoaded(beta);

    EXPECT_TRUE(multi->initialHistorySettled());
    EXPECT_THAT(messageTexts(*multi),
                ::testing::ElementsAre(QStringLiteral("alpha history"),
                                       QStringLiteral("live message"),
                                       QStringLiteral("beta history")));
}

}  // namespace
