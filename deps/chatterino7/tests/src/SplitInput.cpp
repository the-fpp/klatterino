// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageThread.hpp"
#include "messages/Link.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScopeGuard>
#include <QString>
#include <QTextCharFormat>
#include <QTextCursor>

#include <array>
#include <memory>
#include <utility>
#include <vector>

using namespace chatterino;
using ::testing::Exactly;

namespace {

class InputRoutingChannel final : public Channel
{
public:
    explicit InputRoutingChannel(QString name,
                                 Channel::Type type = Channel::Type::Twitch,
                                 QString platform = QStringLiteral("twitch"))
        : Channel(name, type)
    {
        this->context.platform = std::move(platform);
        this->context.channelID = std::move(name);
    }

    bool canSendMessage() const override
    {
        return this->context.authenticated;
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
        if (this->deferSend)
            this->pendingSend = std::move(callback);
        else if (callback)
            callback({SendOutcome::Confirmed, {}});
    }

    MessageSendContext context;
    std::vector<QString> sent;
    bool deferSend = false;
    SendCallback pendingSend;
};

class InputRoutingTwitchIrcServer : public mock::MockTwitchIrcServer
{
public:
    ChannelPtr getOrAddChannel(const QString &name) override
    {
        auto &entry = this->channels[name];
        if (!entry)
        {
            entry = std::make_shared<InputRoutingChannel>(name);
        }
        return entry;
    }

    std::shared_ptr<InputRoutingChannel> get(const QString &name) const
    {
        return this->channels.value(name);
    }

    QHash<QString, std::shared_ptr<InputRoutingChannel>> channels;
};

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args_, this->paths_, this->settings, this->theme,
                        this->fonts)
        , commands(this->paths_)
    {
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    CommandController *getCommands() override
    {
        return &this->commands;
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    BttvEmotes *getBttvEmotes() override
    {
        return &this->bttvEmotes;
    }

    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffzEmotes;
    }

    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventvEmotes;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    InputRoutingTwitchIrcServer twitch;
    mock::EmptyLogging logging;
    HotkeyController hotkeys;
    WindowManager windowManager;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
    BttvEmotes bttvEmotes;
    FfzEmotes ffzEmotes;
    SeventvEmotes seventvEmotes;
};

class TestableSplitInput final : public SplitInput
{
public:
    using SplitInput::handleSendMessage;
    using SplitInput::insertEmotePopupSelection;
    using SplitInput::insertCompletionText;
    using SplitInput::postMessageSend;
    using SplitInput::replyToRecentMentionOrCycle;
    using SplitInput::selectNextMessage;
    using SplitInput::selectPreviousMessage;
    using SplitInput::SplitInput;

    const MessagePtr &replyTarget() const
    {
        return this->replyTarget_;
    }

    ChannelPtr replyChannel() const
    {
        return this->replyChannel_.lock();
    }

    void resetFocusRequests()
    {
        this->focusRequests_ = 0;
    }

    int focusRequests() const
    {
        return this->focusRequests_;
    }

    void setFocusStillAtSubmission(bool value)
    {
        this->focusStillAtSubmission_ = value;
    }

    void resetPopupCloseRequests()
    {
        this->popupCloseRequests_ = 0;
    }

    int popupCloseRequests() const
    {
        return this->popupCloseRequests_;
    }

protected:
    void giveFocus(Qt::FocusReason) override
    {
        ++this->focusRequests_;
    }

    bool shouldRestoreFocus(const QPointer<QWidget> &) const override
    {
        return this->focusStillAtSubmission_;
    }

    void closeEmotePopup() override
    {
        ++this->popupCloseRequests_;
    }

private:
    int focusRequests_ = 0;
    int popupCloseRequests_ = 0;
    bool focusStillAtSubmission_ = true;
};

class MultiChannelSplitInputTest : public ::testing::Test
{
protected:
    MultiChannelSplitInputTest()
        : split(new Split(nullptr))
        , input(this->split)
    {
    }

    void SetUp() override
    {
        const std::array<MultiChannel::Spec, 2> specs{
            MultiChannel::Spec{MultiChannel::Platform::Twitch,
                               QStringLiteral("alpha")},
            MultiChannel::Spec{MultiChannel::Platform::Twitch,
                               QStringLiteral("beta")},
        };
        this->multi = std::make_shared<MultiChannel>(
            specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected);
        this->split->setChannel(IndirectChannel{this->multi});
        QCoreApplication::processEvents();
    }

    std::shared_ptr<InputRoutingChannel> channel(const QString &name) const
    {
        return this->mockApplication.twitch.get(name);
    }

    static void setSendable(const std::shared_ptr<InputRoutingChannel> &channel,
                            bool sendable)
    {
        channel->context.writable = sendable;
        channel->context.authenticated = sendable;
    }

    QString send()
    {
        return this->input.handleSendMessage({});
    }

    MockApplication mockApplication;
    Split *split;
    TestableSplitInput input;
    std::shared_ptr<MultiChannel> multi;
};

class SingleChannelFocusTest : public ::testing::Test
{
protected:
    SingleChannelFocusTest()
        : split(new Split(nullptr))
        , input(this->split)
    {
    }

    void setChannel(Channel::Type type, const QString &platform)
    {
        this->channel = std::make_shared<InputRoutingChannel>(
            QStringLiteral("focus-room"), type, platform);
        this->channel->context.writable = true;
        this->channel->context.authenticated = true;
        this->split->setChannel(IndirectChannel{this->channel});
        QCoreApplication::processEvents();
    }

    QString send()
    {
        return this->input.handleSendMessage({});
    }

    MockApplication mockApplication;
    Split *split;
    TestableSplitInput input;
    std::shared_ptr<InputRoutingChannel> channel;
};

class SplitInputTest
    : public ::testing::TestWithParam<std::tuple<QString, QString>>
{
public:
    SplitInputTest()
        : split(new Split(nullptr))
        , input(this->split)
    {
    }

    MockApplication mockApplication;
    Split *split;
    SplitInput input;
};

MessagePtr makeHistorySelectionMessage(const QString &text)
{
    auto message = std::make_shared<Message>();
    message->messageText = text;
    message->searchText = text;
    return message;
}

MessagePtr makeRumbleHistorySelectionMessage(const QString &text,
                                             const QString &channelName)
{
    auto message = std::make_shared<Message>();
    message->id = text;
    message->loginName = QStringLiteral("viewer");
    message->displayName = QStringLiteral("Viewer");
    message->channelName = channelName;
    message->messageText = text;
    message->searchText = text;
    message->platform = MessagePlatform::Rumble;
    message->serverReceivedTime = QDateTime::currentDateTime();
    return message;
}

MessagePtrMut makeReplyHistoryMessage(const QString &id, const QString &name,
                                      const QString &userID,
                                      bool showInMentions = false,
                                      const QString &channelName =
                                          QStringLiteral("test"))
{
    auto message = std::make_shared<Message>();
    message->id = id;
    message->loginName = name.toLower();
    message->displayName = name;
    message->userID = userID;
    message->channelName = channelName;
    message->messageText = id;
    message->serverReceivedTime = QDateTime::currentDateTime();
    if (showInMentions)
    {
        message->flags.set(MessageFlag::ShowInMentions);
    }
    return message;
}

void sendKey(QWidget &target, int key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier,
             const QString &text = {})
{
    QKeyEvent shortcutOverride(QEvent::ShortcutOverride, key, modifiers, text);
    QApplication::sendEvent(&target, &shortcutOverride);

    QKeyEvent keyPress(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(&target, &keyPress);

    QKeyEvent keyRelease(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(&target, &keyRelease);
    QCoreApplication::processEvents();
}

class MessageHistorySelectionTest : public ::testing::Test
{
protected:
    MessageHistorySelectionTest()
        : split(new Split(nullptr))
        , channel(std::make_shared<Channel>(QStringLiteral("test"),
                                            Channel::Type::None))
    {
    }

    void SetUp() override
    {
        this->split->resize(800, 600);
        this->split->getChannelView().setChannel(this->channel);
        QCoreApplication::processEvents();

        auto *textEdit = this->split->findChild<QTextEdit *>();
        this->editor = dynamic_cast<ResizingTextEdit *>(textEdit);
        ASSERT_NE(this->editor, nullptr);
        this->editor->setEnabled(true);
        QCoreApplication::processEvents();
    }

    void addMessage(const QString &text)
    {
        this->channel->addMessage(makeHistorySelectionMessage(text),
                                  MessageContext::Original);
        QCoreApplication::processEvents();
    }

    void setChannel(ChannelPtr replacement)
    {
        this->channel = std::move(replacement);
        this->split->setChannel(IndirectChannel{this->channel});
        QCoreApplication::processEvents();
    }

    void addMessage(const MessagePtr &message)
    {
        this->channel->addMessage(message, MessageContext::Original);
        QCoreApplication::processEvents();
    }

    void activateSelection()
    {
        sendKey(*this->editor, Qt::Key_K, Qt::AltModifier);
    }

    void copySelectionToInput()
    {
        sendKey(*this->editor, Qt::Key_Return, Qt::ControlModifier);
    }

    MockApplication mockApplication;
    Split *split;
    ChannelPtr channel;
    ResizingTextEdit *editor = nullptr;
};

class ReplyHistorySelectionTest : public ::testing::Test
{
protected:
    ReplyHistorySelectionTest()
        : split(new Split(nullptr))
        , channel(std::make_shared<InputRoutingChannel>(QStringLiteral("test")))
    {
        this->channel->context.accountID = QStringLiteral("self-id");
        this->channel->context.authenticated = true;
        this->channel->context.writable = true;
    }

    void SetUp() override
    {
        this->split->resize(800, 600);
        this->split->setChannel(IndirectChannel{this->channel});
        QCoreApplication::processEvents();

        auto *textEdit = this->split->findChild<QTextEdit *>();
        this->editor = dynamic_cast<ResizingTextEdit *>(textEdit);
        ASSERT_NE(this->editor, nullptr);
        this->editor->setEnabled(true);
        QCoreApplication::processEvents();
    }

    void addMessage(const MessagePtr &message)
    {
        this->channel->addMessage(message, MessageContext::Original);
        QCoreApplication::processEvents();
    }

    MockApplication mockApplication;
    Split *split;
    std::shared_ptr<InputRoutingChannel> channel;
    ResizingTextEdit *editor = nullptr;
};

}  // namespace

TEST_P(SplitInputTest, Reply)
{
    std::tuple<QString, QString> params = this->GetParam();
    auto [inputText, expected] = params;
    ASSERT_EQ("", this->input.getInputText());
    this->input.setInputText(inputText);
    ASSERT_EQ(inputText, this->input.getInputText());

    auto *message = new Message();
    message->displayName = "forsen";
    auto reply = MessagePtr(message);
    this->input.setReply(reply, {});
    QString actual = this->input.getInputText();
    ASSERT_EQ(expected, actual) << "Input text after setReply should be '"
                                << expected << "', but got '" << actual << "'";
}

INSTANTIATE_TEST_SUITE_P(
    SplitInput, SplitInputTest,
    testing::Values(
        // Ensure message is retained
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen,",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with comma is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma and space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen, ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure it works with no message
        std::make_tuple<QString, QString>(
            "",
            "@forsen ")));

TEST_F(MultiChannelSplitInputTest, NoDestinationPreservesInput)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, false);
    this->input.setInputText(QStringLiteral("keep this draft"));

    const auto failure = this->send();

    EXPECT_FALSE(failure.isEmpty());
    EXPECT_TRUE(failure.contains(QStringLiteral("alpha")));
    EXPECT_TRUE(failure.contains(QStringLiteral("beta")));
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("keep this draft"));
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest,
       CompatibleFallbackSendsOnceWithoutChangingActiveChild)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);
    ASSERT_EQ(this->multi->activeChannelIndex(), 0U);
    this->input.setInputText(QStringLiteral("route me"));

    EXPECT_TRUE(this->send().isEmpty());
    QCoreApplication::processEvents();

    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("route me")));
    EXPECT_EQ(this->input.focusRequests(), 1);
    EXPECT_EQ(this->multi->activeChannelIndex(), 0U);
    EXPECT_TRUE(this->input.getInputText().isEmpty());
    const auto feedback = this->multi->getLastMessage();
    EXPECT_TRUE(feedback == nullptr ||
                !feedback->messageText.contains(QStringLiteral("Sent via")));
}

TEST_F(MultiChannelSplitInputTest,
       AsyncConfirmedClearsButDefiniteAndAmbiguousPreserveDraft)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    alpha->deferSend = true;

    this->input.setInputText(QStringLiteral("confirmed"));
    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("confirmed"));
    ASSERT_TRUE(alpha->pendingSend);
    auto confirmed = std::move(alpha->pendingSend);
    confirmed({Channel::SendOutcome::Confirmed, {}});
    EXPECT_TRUE(this->input.getInputText().isEmpty());

    for (const auto outcome : {Channel::SendOutcome::DefiniteFailure,
                               Channel::SendOutcome::Ambiguous})
    {
        this->input.setInputText(QStringLiteral("preserve"));
        EXPECT_TRUE(this->send().isEmpty());
        ASSERT_TRUE(alpha->pendingSend);
        auto pending = std::move(alpha->pendingSend);
        pending({outcome, QStringLiteral("safe failure")});
        EXPECT_EQ(this->input.getInputText(), QStringLiteral("preserve"));
    }
}

TEST_F(MultiChannelSplitInputTest,
       EditThenRevertDoesNotLetConfirmedCompletionClearNewRevision)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    alpha->deferSend = true;
    this->input.setInputText(QStringLiteral("same text"));
    EXPECT_TRUE(this->send().isEmpty());
    ASSERT_TRUE(alpha->pendingSend);

    this->input.setInputText(QStringLiteral("temporary edit"));
    this->input.setInputText(QStringLiteral("same text"));
    auto pending = std::move(alpha->pendingSend);
    pending({Channel::SendOutcome::Confirmed, {}});
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("same text"));
}

TEST_F(MultiChannelSplitInputTest, SubmitTimeActiveChildIsPrimary)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    this->input.setInputText(QStringLiteral("current primary"));

    this->multi->setActiveChannelIndex(1);
    this->input.resetFocusRequests();
    EXPECT_TRUE(this->send().isEmpty());
    QCoreApplication::processEvents();

    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("current primary")));
    EXPECT_EQ(this->input.focusRequests(), 1);
}

TEST_F(MultiChannelSplitInputTest, UnknownCommandNeverFallsBack)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);
    this->input.setInputText(QStringLiteral("/not-a-chatterino-command"));

    const auto failure = this->send();

    EXPECT_FALSE(failure.isEmpty());
    EXPECT_EQ(this->input.getInputText(),
              QStringLiteral("/not-a-chatterino-command"));
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest,
       CompletedLocalCommandDoesNotSendAnEmptyProviderMessage)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);
    this->input.setInputText(QStringLiteral("/clear"));

    EXPECT_TRUE(this->send().isEmpty());

    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST_F(MultiChannelSplitInputTest,
       TypedEmoteProvenanceSelectsItsCurrentCompatibleChild)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;
    beta->context.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("only-beta-id")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };
    beta->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":only"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    ASSERT_EQ(this->input.getInputText(), QStringLiteral("OnlyBeta "));

    EXPECT_TRUE(this->send().isEmpty());

    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("OnlyBeta ")));
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST_F(MultiChannelSplitInputTest,
       ConfirmedCompletionSelectedEmoteReturnsFocusToMessageEntry)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    alpha->context.emoteCapabilitiesComplete = true;
    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("native"),
            .id = EmoteId{QStringLiteral("focus-emote")},
        },
        .insertionText = QStringLiteral("FocusEmote"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("alpha"),
        },
    };
    alpha->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":focus"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    this->input.resetFocusRequests();
    EXPECT_TRUE(this->send().isEmpty());
    QCoreApplication::processEvents();

    EXPECT_EQ(this->input.focusRequests(), 1);
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST_F(MultiChannelSplitInputTest, RejectedSendPreservesEditorFocusAndDraft)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, false);
    this->input.setInputText(QStringLiteral("unsent draft"));

    EXPECT_FALSE(this->send().isEmpty());
    QCoreApplication::processEvents();

    EXPECT_EQ(this->input.focusRequests(), 0);
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("unsent draft"));
}

TEST_F(MultiChannelSplitInputTest, ConfirmedPlainTextSendRetainsEditorFocus)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    this->input.setInputText(QStringLiteral("plain text"));
    this->input.resetFocusRequests();

    EXPECT_TRUE(this->send().isEmpty());
    QCoreApplication::processEvents();

    EXPECT_EQ(this->input.focusRequests(), 1);
}

TEST_F(MultiChannelSplitInputTest,
       EmotePickerSelectionClosesPopupAndKeepsEditorAvailable)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    this->input.resetFocusRequests();
    this->input.resetPopupCloseRequests();
    this->input.insertEmotePopupSelection(
        Link{Link::InsertText, QStringLiteral("PickerEmote")}, std::nullopt,
        false);

    EXPECT_EQ(this->input.popupCloseRequests(), 1);
    EXPECT_EQ(this->input.focusRequests(), 1);
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("PickerEmote "));
}

TEST_F(MultiChannelSplitInputTest,
       DelayedConfirmationDoesNotStealFocusFromAnotherWidget)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    alpha->deferSend = true;
    this->input.setInputText(QStringLiteral("delayed"));

    EXPECT_TRUE(this->send().isEmpty());
    ASSERT_TRUE(alpha->pendingSend);

    this->input.setFocusStillAtSubmission(false);

    auto pending = std::move(alpha->pendingSend);
    pending({Channel::SendOutcome::Confirmed, {}});
    QCoreApplication::processEvents();

    EXPECT_EQ(this->input.focusRequests(), 0);
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST_F(SingleChannelFocusTest,
       ConfirmedNativeAndThirdPartyEmotesRestoreFocusAcrossProviders)
{
    const std::array providers{
        std::pair{Channel::Type::Twitch, QStringLiteral("twitch")},
        std::pair{Channel::Type::Kick, QStringLiteral("kick")},
        std::pair{Channel::Type::Rumble, QStringLiteral("rumble")},
    };
    const std::array drafts{QStringLiteral("NativeEmote"),
                            QStringLiteral("ThirdPartyEmote")};

    for (const auto &[type, platform] : providers)
    {
        for (const auto &draft : drafts)
        {
            this->setChannel(type, platform);
            this->input.setInputText(draft);
            this->input.resetFocusRequests();

            EXPECT_TRUE(this->send().isEmpty()) << platform << ": " << draft;
            QCoreApplication::processEvents();

            EXPECT_EQ(this->input.focusRequests(), 1)
                << platform << ": " << draft;
            EXPECT_TRUE(this->input.getInputText().isEmpty());
        }
    }
}

TEST_F(SingleChannelFocusTest, FailedSendPreservesDraftWithoutRefocusing)
{
    this->setChannel(Channel::Type::Rumble, QStringLiteral("rumble"));
    this->channel->deferSend = true;
    this->input.setInputText(QStringLiteral("ThirdPartyEmote"));

    EXPECT_TRUE(this->send().isEmpty());
    ASSERT_TRUE(this->channel->pendingSend);

    auto pending = std::move(this->channel->pendingSend);
    pending({Channel::SendOutcome::DefiniteFailure,
             QStringLiteral("safe failure")});
    QCoreApplication::processEvents();

    EXPECT_EQ(this->input.focusRequests(), 0);
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("ThirdPartyEmote"));
}

TEST_F(MultiChannelSplitInputTest,
       FormattingOnlyChangePreservesTypedEmoteProvenance)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;
    beta->context.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("only-beta-id")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };
    beta->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":only"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    auto *editor = dynamic_cast<ResizingTextEdit *>(
        this->input.findChild<QTextEdit *>());
    ASSERT_NE(editor, nullptr);

    QTextCursor cursor(editor->document());
    cursor.setPosition(1);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    QTextCharFormat format;
    format.setFontItalic(true);
    cursor.mergeCharFormat(format);
    QCoreApplication::processEvents();

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("OnlyBeta ")));
}

TEST_F(MultiChannelSplitInputTest,
       ZeroLengthFormattingNotificationPreservesTypedEmoteProvenance)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;
    beta->context.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("only-beta-id")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };
    beta->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":only"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    auto *editor = dynamic_cast<ResizingTextEdit *>(
        this->input.findChild<QTextEdit *>());
    ASSERT_NE(editor, nullptr);

    // QSyntaxHighlighter marks layout-format ranges dirty without changing
    // either the plain text or the document's serialized HTML.
    editor->document()->contentsChange(1, 0, 0);

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("OnlyBeta ")));
}

TEST_F(MultiChannelSplitInputTest,
       OverflowTrimmingRetainsCompleteTypedEmoteProvenance)
{
    const auto previousOverflow =
        this->mockApplication.settings.messageOverflow.getValue();
    this->mockApplication.settings.messageOverflow.setValue(
        MessageOverflow::Prevent);
    const auto restoreOverflow = qScopeGuard([this, previousOverflow] {
        this->mockApplication.settings.messageOverflow.setValue(
            previousOverflow);
    });

    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;
    beta->context.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("only-beta-id")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };
    beta->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":only"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    this->input.insertText(QString(500, u'x'));
    const auto expected =
        QStringLiteral("OnlyBeta ") + QString(491, u'x');
    ASSERT_EQ(this->input.getInputText(), expected);

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent, ::testing::ElementsAre(expected));
}

TEST_F(MultiChannelSplitInputTest,
       OverflowTrimmingRejectsPartialTypedEmote)
{
    const auto previousOverflow =
        this->mockApplication.settings.messageOverflow.getValue();
    this->mockApplication.settings.messageOverflow.setValue(
        MessageOverflow::Prevent);
    const auto restoreOverflow = qScopeGuard([this, previousOverflow] {
        this->mockApplication.settings.messageOverflow.setValue(
            previousOverflow);
    });

    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("only-beta-id")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };

    this->input.insertText(QString(495, u'x') + QStringLiteral(":only"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    const auto beforeSend = this->input.getInputText();
    ASSERT_EQ(beforeSend.size(), 500);
    EXPECT_TRUE(beforeSend.endsWith(QStringLiteral("OnlyB")));

    const auto failure = this->send();
    EXPECT_TRUE(failure.contains(QStringLiteral("lost its stable provenance")));
    EXPECT_EQ(this->input.getInputText(), beforeSend);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest,
       IdenticalTextReplacementReconstructsTypedEmoteFromUnion)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;
    beta->context.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("only-beta-id")},
        },
        .insertionText = QStringLiteral("OnlyBeta"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };
    beta->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":only"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    auto *editor = dynamic_cast<ResizingTextEdit *>(
        this->input.findChild<QTextEdit *>());
    ASSERT_NE(editor, nullptr);

    QTextCursor cursor(editor->document());
    cursor.setPosition(0);
    cursor.setPosition(selected.insertionText.size(),
                       QTextCursor::KeepAnchor);
    cursor.insertText(selected.insertionText);
    QCoreApplication::processEvents();

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("OnlyBeta ")));
}

TEST_F(MultiChannelSplitInputTest,
       LiveReplyCapabilityFailurePreservesInputAndBinding)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;

    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input.setReply(reply, alpha);

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("removed-id")},
        },
        .insertionText = QStringLiteral("RemovedNow"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("alpha"),
        },
    };
    this->input.insertText(QStringLiteral(":removed"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    const auto beforeSend = this->input.getInputText();

    const auto failure = this->send();

    EXPECT_TRUE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_TRUE(failure.contains(QStringLiteral("unavailable")));
    EXPECT_EQ(this->input.getInputText(), beforeSend);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest,
       PlainReplyPrefixRewriteDoesNotPoisonDraft)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    this->input.setInputText(QStringLiteral("plain reply"));

    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input.setReply(reply, alpha);
    const auto replyText = QStringLiteral("@viewer plain reply ");
    ASSERT_EQ(this->input.getInputText(), replyText);

    // The fake is intentionally not a concrete provider channel. Reaching its
    // provider boundary with no failure proves the plain rebased draft did not
    // retain a paragraph-separator invalidation from setPlainText().
    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_EQ(this->input.getInputText(), replyText);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());

    // The reply binding remains current and still evaluates live state.
    setSendable(alpha, false);
    const auto failure = this->send();
    EXPECT_TRUE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_TRUE(failure.contains(QStringLiteral("not writable")));
    EXPECT_EQ(this->input.getInputText(), replyText);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest,
       LiveReplyPrefixRewritePreservesCurrentTypedEmote)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("current-id")},
        },
        .insertionText = QStringLiteral("CurrentNow"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("alpha"),
        },
    };
    alpha->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });

    this->input.insertText(QStringLiteral(":current"));
    this->input.insertCompletionText(selected.insertionText, selected, false);

    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input.setReply(reply, alpha);
    ASSERT_EQ(this->input.getInputText(),
              QStringLiteral("@viewer CurrentNow "));

    auto replacementReply = std::make_shared<Message>();
    replacementReply->id = QStringLiteral("replacement-reply-id");
    replacementReply->displayName = QStringLiteral("moderator");
    this->input.setReply(replacementReply, alpha);
    const auto replyText = QStringLiteral("@moderator CurrentNow ");
    ASSERT_EQ(this->input.getInputText(), replyText);

    // The fake child is intentionally not a concrete provider channel. An
    // empty result with unchanged input proves live evaluation succeeded and
    // reached that provider boundary without losing the selected identity.
    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_EQ(this->input.getInputText(), replyText);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());

    // The binding and provenance remain live: removing the capability now
    // rejects the same draft instead of silently treating it as plain text.
    alpha->context.emoteCapabilities.clear();
    const auto failure = this->send();
    EXPECT_TRUE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_TRUE(failure.contains(QStringLiteral("unavailable")));
    EXPECT_EQ(this->input.getInputText(), replyText);
}

TEST_F(MultiChannelSplitInputTest,
       LiveReplyDoesNotFallbackToCompatibleSibling)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.emoteCapabilitiesComplete = true;
    beta->context.emoteCapabilitiesComplete = true;

    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input.setReply(reply, alpha);

    const DraftEmoteCandidate selected{
        .identity = {
            .provider = QStringLiteral("7tv"),
            .id = EmoteId{QStringLiteral("beta-only-id")},
        },
        .insertionText = QStringLiteral("BetaOnly"),
        .availability = {
            .platform = QStringLiteral("twitch"),
            .channelID = QStringLiteral("beta"),
        },
    };
    beta->context.emoteCapabilities.push_back({
        .identity = selected.identity,
        .insertionText = selected.insertionText,
        .availability = selected.availability,
    });
    this->input.insertText(QStringLiteral(":beta"));
    this->input.insertCompletionText(selected.insertionText, selected, false);
    const auto beforeSend = this->input.getInputText();

    const auto failure = this->send();

    EXPECT_TRUE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_TRUE(failure.contains(QStringLiteral("unavailable")));
    EXPECT_EQ(this->input.getInputText(), beforeSend);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest, ExpiredReplyDestinationPreservesInput)
{
    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input.setInputText(QStringLiteral("reply body"));
    this->input.setReply(reply, std::weak_ptr<Channel>{});
    const auto beforeSend = this->input.getInputText();

    const auto failure = this->send();

    EXPECT_FALSE(failure.isEmpty());
    EXPECT_EQ(this->input.getInputText(), beforeSend);
    EXPECT_TRUE(this->channel(QStringLiteral("alpha"))->sent.empty());
    EXPECT_TRUE(this->channel(QStringLiteral("beta"))->sent.empty());
}

TEST_F(MultiChannelSplitInputTest, ReplyHistoryStripsOnlyBoundaryTags)
{
    struct Case {
        QString recalled;
        QString body;
    };
    const std::vector<Case> cases{
        {QStringLiteral("@Bob previous message"),
         QStringLiteral("previous message")},
        {QStringLiteral("previous message @Bob"),
         QStringLiteral("previous message")},
        {QStringLiteral("@Bob previous message @Carol"),
         QStringLiteral("previous message")},
        {QStringLiteral("  @Bob,   @Carol repeated   body @Dave @Eve,  "),
         QStringLiteral("repeated   body")},
        {QStringLiteral("previous @Bob message"),
         QStringLiteral("previous @Bob message")},
        {QStringLiteral("@Bob @Carol"), QString()},
        {QStringLiteral("@not/a-tag ordinary"),
         QStringLiteral("@not/a-tag ordinary")},
    };
    for (const auto &test : cases)
    {
        this->input.postMessageSend(test.recalled, {});
    }

    auto target = std::make_shared<Message>();
    target->id = QStringLiteral("reply-id");
    target->displayName = QStringLiteral("Alice");
    this->input.setInputText(QStringLiteral("original draft"));
    this->input.setReply(target, this->channel(QStringLiteral("alpha")));

    for (auto it = cases.rbegin(); it != cases.rend(); ++it)
    {
        SCOPED_TRACE(it->recalled);
        this->input.selectPreviousMessage();
        const auto expected = it->body.isEmpty()
                                  ? QStringLiteral("@Alice ")
                                  : QStringLiteral("@Alice ") + it->body;
        EXPECT_EQ(this->input.getInputText(), expected);
        EXPECT_EQ(this->input.replyTarget(), target);
    }
}

TEST_F(MultiChannelSplitInputTest,
       ReplyHistoryCyclesForwardRestoresDraftAndStoredEntries)
{
    this->input.postMessageSend(QStringLiteral("@Older older body @Trailing"),
                                {});
    this->input.postMessageSend(QStringLiteral("@Newer newer body"), {});

    auto target = std::make_shared<Message>();
    target->id = QStringLiteral("reply-id");
    target->displayName = QStringLiteral("Alice");
    this->input.setInputText(QStringLiteral("original draft"));
    this->input.setReply(target, this->channel(QStringLiteral("alpha")));
    const auto original = QStringLiteral("@Alice original draft ");
    ASSERT_EQ(this->input.getInputText(), original);

    this->input.selectPreviousMessage();
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("@Alice newer body"));
    this->input.selectPreviousMessage();
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("@Alice older body"));

    this->input.setInputText(QStringLiteral("@Alice edited recalled body"));
    ASSERT_EQ(this->input.replyTarget(), target);

    this->input.selectNextMessage();
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("@Alice newer body"));
    this->input.selectNextMessage();
    EXPECT_EQ(this->input.getInputText(), original);
    EXPECT_EQ(this->input.replyTarget(), target);

    // Editing a recalled value never mutates the stored entry.
    this->input.selectPreviousMessage();
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("@Alice newer body"));
    this->input.selectNextMessage();
    EXPECT_EQ(this->input.getInputText(), original);

    this->input.setReply(nullptr, {});
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("original draft"));
    EXPECT_EQ(this->input.replyTarget(), nullptr);
}

TEST_F(MultiChannelSplitInputTest, RecalledReplyStaysBoundToOriginalDestination)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    this->input.postMessageSend(QStringLiteral("@Other recalled body"), {});

    auto target = std::make_shared<Message>();
    target->id = QStringLiteral("reply-id");
    target->displayName = QStringLiteral("Alice");
    this->input.setReply(target, alpha);
    this->input.selectPreviousMessage();
    ASSERT_EQ(this->input.getInputText(),
              QStringLiteral("@Alice recalled body"));
    ASSERT_EQ(this->input.replyChannel(), alpha);

    setSendable(alpha, false);
    const auto failure = this->send();

    EXPECT_TRUE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_TRUE(failure.contains(QStringLiteral("alpha")));
    EXPECT_EQ(this->input.getInputText(),
              QStringLiteral("@Alice recalled body"));
    EXPECT_EQ(this->input.replyTarget(), target);
    EXPECT_EQ(this->input.replyChannel(), alpha);
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
}

TEST_F(MultiChannelSplitInputTest, OrdinaryHistoryNavigationIsUnchanged)
{
    this->input.postMessageSend(QStringLiteral("@Bob raw history"), {});
    this->input.setInputText(QStringLiteral("current draft"));

    this->input.selectPreviousMessage();
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("@Bob raw history"));
    EXPECT_EQ(this->input.replyTarget(), nullptr);

    this->input.selectNextMessage();
    EXPECT_EQ(this->input.getInputText(), QStringLiteral("current draft"));
    EXPECT_EQ(this->input.replyTarget(), nullptr);
}

TEST_F(MessageHistorySelectionTest, FirstAltKSelectsNewestMessage)
{
    this->addMessage(QStringLiteral("older message"));
    this->addMessage(QStringLiteral("newest message"));

    this->activateSelection();
    this->copySelectionToInput();

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("newest message"));
}

TEST_F(ReplyHistorySelectionTest, BackspaceRestoresIncomingReplyTarget)
{
    auto target = makeReplyHistoryMessage(QStringLiteral("incoming-id"),
                                          QStringLiteral("Alice"),
                                          QStringLiteral("alice-id"), true);
    this->addMessage(target);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Alice "));
}

TEST_F(ReplyHistorySelectionTest,
       BackspaceRestoresParentAfterOutgoingReplyActivity)
{
    auto target = makeReplyHistoryMessage(QStringLiteral("target-id"),
                                          QStringLiteral("Alice"),
                                          QStringLiteral("alice-id"));
    auto outgoing = makeReplyHistoryMessage(QStringLiteral("outgoing-id"),
                                             QStringLiteral("Me"),
                                             QStringLiteral("self-id"));
    auto consecutive = makeReplyHistoryMessage(
        QStringLiteral("consecutive-id"), QStringLiteral("Me"),
        QStringLiteral("self-id"));
    outgoing->replyParent = target;
    consecutive->replyParent = target;
    this->addMessage(target);
    this->addMessage(outgoing);
    this->addMessage(consecutive);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Alice "));
}

TEST_F(ReplyHistorySelectionTest,
       BackspaceChoosesMostRecentEligibleThreadActivity)
{
    auto olderTarget = makeReplyHistoryMessage(QStringLiteral("older-id"),
                                               QStringLiteral("Older"),
                                               QStringLiteral("older-id"),
                                               true);
    auto newerTarget = makeReplyHistoryMessage(QStringLiteral("newer-id"),
                                               QStringLiteral("Newer"),
                                               QStringLiteral("newer-id"));
    auto outgoing = makeReplyHistoryMessage(QStringLiteral("outgoing-id"),
                                             QStringLiteral("Me"),
                                             QStringLiteral("self-id"));
    outgoing->replyParent = newerTarget;
    this->addMessage(olderTarget);
    this->addMessage(newerTarget);
    this->addMessage(outgoing);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Newer "));
}

TEST_F(ReplyHistorySelectionTest,
       BackspaceUsesImmediateParentInsteadOfThreadRoot)
{
    auto root = makeReplyHistoryMessage(QStringLiteral("root-id"),
                                        QStringLiteral("Root"),
                                        QStringLiteral("root-user"));
    auto parent = makeReplyHistoryMessage(QStringLiteral("parent-id"),
                                          QStringLiteral("Parent"),
                                          QStringLiteral("parent-user"));
    auto outgoing = makeReplyHistoryMessage(QStringLiteral("outgoing-id"),
                                             QStringLiteral("Me"),
                                             QStringLiteral("self-id"));
    auto thread = std::make_shared<MessageThread>(root);
    parent->replyParent = root;
    parent->replyThread = thread;
    outgoing->replyParent = parent;
    outgoing->replyThread = thread;
    thread->addToThread(std::static_pointer_cast<const Message>(parent));
    thread->addToThread(std::static_pointer_cast<const Message>(outgoing));
    this->addMessage(root);
    this->addMessage(parent);
    this->addMessage(outgoing);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Parent "));
}

TEST_F(ReplyHistorySelectionTest,
       BackspaceRejectsWrongUserAndSelfParentActivity)
{
    auto fallback = makeReplyHistoryMessage(QStringLiteral("fallback-id"),
                                            QStringLiteral("Fallback"),
                                            QStringLiteral("fallback-user"),
                                            true);
    this->addMessage(fallback);

    auto wrongUserParent = makeReplyHistoryMessage(
        QStringLiteral("wrong-parent"), QStringLiteral("Wrong"),
        QStringLiteral("wrong-user"));
    auto wrongUser = makeReplyHistoryMessage(QStringLiteral("wrong-outgoing"),
                                             QStringLiteral("Other"),
                                             QStringLiteral("other-id"));
    wrongUser->replyParent = wrongUserParent;
    this->addMessage(wrongUser);

    auto selfParent = makeReplyHistoryMessage(QStringLiteral("self-parent"),
                                              QStringLiteral("Me"),
                                              QStringLiteral("self-id"));
    auto selfReply = makeReplyHistoryMessage(QStringLiteral("self-outgoing"),
                                             QStringLiteral("Me"),
                                             QStringLiteral("self-id"));
    selfReply->replyParent = selfParent;
    this->addMessage(selfReply);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Fallback "));
}

TEST_F(ReplyHistorySelectionTest, BackspaceRejectsLoggedOutOutgoingActivity)
{
    auto fallback = makeReplyHistoryMessage(QStringLiteral("fallback-id"),
                                            QStringLiteral("Fallback"),
                                            QStringLiteral("fallback-user"),
                                            true);
    auto outgoing = makeReplyHistoryMessage(QStringLiteral("outgoing-id"),
                                             QStringLiteral("Me"),
                                             QStringLiteral("self-id"));
    outgoing->replyParent = makeReplyHistoryMessage(
        QStringLiteral("parent-id"), QStringLiteral("Parent"),
        QStringLiteral("parent-user"));
    this->addMessage(fallback);
    this->addMessage(outgoing);

    this->channel->context.authenticated = false;
    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Fallback "));
}

TEST_F(ReplyHistorySelectionTest, BackspaceRecognizesKickAccountID)
{
    auto kick = std::make_shared<InputRoutingChannel>(
        QStringLiteral("kick-room"), Channel::Type::Kick,
        QStringLiteral("kick"));
    kick->context.accountID = QStringLiteral("kick-account-id");
    kick->context.authenticated = true;
    kick->context.writable = true;
    this->split->setChannel(IndirectChannel{kick});
    QCoreApplication::processEvents();

    auto target = makeReplyHistoryMessage(QStringLiteral("kick-target"),
                                          QStringLiteral("KickTarget"),
                                          QStringLiteral("other-kick-id"),
                                          false, QStringLiteral("kick-room"));
    target->platform = MessagePlatform::Kick;
    auto outgoing = makeReplyHistoryMessage(
        QStringLiteral("kick-outgoing"), QStringLiteral("DifferentLogin"),
        QStringLiteral("kick-account-id"), false, QStringLiteral("kick-room"));
    outgoing->platform = MessagePlatform::Kick;
    outgoing->replyParent = target;
    kick->addMessage(target, MessageContext::Original);
    kick->addMessage(outgoing, MessageContext::Original);
    QCoreApplication::processEvents();

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@KickTarget "));
}

TEST_F(ReplyHistorySelectionTest,
       BackspaceRejectsInvalidActivityAndInvalidParents)
{
    auto fallback = makeReplyHistoryMessage(QStringLiteral("fallback-id"),
                                            QStringLiteral("Fallback"),
                                            QStringLiteral("fallback-user"),
                                            true);
    this->addMessage(fallback);

    const std::array invalidFlags{
        MessageFlag::System,
        MessageFlag::Disabled,
        MessageFlag::Timeout,
        MessageFlag::InvalidReplyTarget,
    };
    for (size_t i = 0; i < invalidFlags.size(); ++i)
    {
        auto parent = makeReplyHistoryMessage(
            QStringLiteral("activity-parent-%1").arg(i),
            QStringLiteral("ActivityParent"),
            QStringLiteral("activity-parent-user"));
        auto outgoing = makeReplyHistoryMessage(
            QStringLiteral("invalid-activity-%1").arg(i),
            QStringLiteral("Me"), QStringLiteral("self-id"));
        outgoing->flags.set(invalidFlags[i]);
        outgoing->replyParent = parent;
        this->addMessage(outgoing);

        auto invalidParent = makeReplyHistoryMessage(
            QStringLiteral("invalid-parent-%1").arg(i),
            QStringLiteral("InvalidParent"),
            QStringLiteral("invalid-parent-user"));
        invalidParent->flags.set(invalidFlags[i]);
        auto validActivity = makeReplyHistoryMessage(
            QStringLiteral("parent-activity-%1").arg(i),
            QStringLiteral("Me"), QStringLiteral("self-id"));
        validActivity->replyParent = invalidParent;
        this->addMessage(validActivity);
    }

    auto emptyParent = makeReplyHistoryMessage(
        QString(), QStringLiteral("Empty"), QStringLiteral("empty-user"));
    auto emptyTargetActivity = makeReplyHistoryMessage(
        QStringLiteral("empty-target-activity"), QStringLiteral("Me"),
        QStringLiteral("self-id"));
    emptyTargetActivity->replyParent = emptyParent;
    this->addMessage(emptyTargetActivity);

    auto emptyActivity = makeReplyHistoryMessage(
        QString(), QStringLiteral("Me"), QStringLiteral("self-id"));
    emptyActivity->replyParent = makeReplyHistoryMessage(
        QStringLiteral("empty-activity-parent"), QStringLiteral("Parent"),
        QStringLiteral("parent-user"));
    this->addMessage(emptyActivity);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Fallback "));
}

TEST_F(ReplyHistorySelectionTest, BackspaceRejectsActivityThatIsNotReplyable)
{
    auto fallback = makeReplyHistoryMessage(
        QStringLiteral("fallback-id"), QStringLiteral("Fallback"),
        QStringLiteral("fallback-user"), true);
    auto parent = makeReplyHistoryMessage(QStringLiteral("parent-id"),
                                          QStringLiteral("Parent"),
                                          QStringLiteral("parent-user"));
    auto outgoing = makeReplyHistoryMessage(QStringLiteral("outgoing-id"),
                                            QStringLiteral("Me"),
                                            QStringLiteral("self-id"));
    outgoing->flags.set(MessageFlag::Subscription);
    outgoing->replyParent = parent;
    ASSERT_EQ(outgoing->isReplyable(), Message::ReplyStatus::NotReplyable);
    this->addMessage(fallback);
    this->addMessage(outgoing);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Fallback "));
}

TEST_F(ReplyHistorySelectionTest, BackspaceRejectsParentThatIsNotReplyable)
{
    auto fallback = makeReplyHistoryMessage(
        QStringLiteral("fallback-id"), QStringLiteral("Fallback"),
        QStringLiteral("fallback-user"), true);
    auto parent = makeReplyHistoryMessage(QStringLiteral("parent-id"),
                                          QStringLiteral("Parent"),
                                          QStringLiteral("parent-user"));
    parent->flags.set(MessageFlag::Whisper);
    auto outgoing = makeReplyHistoryMessage(QStringLiteral("outgoing-id"),
                                            QStringLiteral("Me"),
                                            QStringLiteral("self-id"));
    outgoing->replyParent = parent;
    ASSERT_EQ(parent->isReplyable(), Message::ReplyStatus::NotReplyable);
    this->addMessage(fallback);
    this->addMessage(outgoing);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Fallback "));
}

TEST_F(ReplyHistorySelectionTest,
       BackspaceDeduplicatesRepeatedActivityButStillCyclesOlderTargets)
{
    auto older = makeReplyHistoryMessage(QStringLiteral("older-id"),
                                         QStringLiteral("Older"),
                                         QStringLiteral("older-user"), true);
    auto recent = makeReplyHistoryMessage(QStringLiteral("recent-id"),
                                          QStringLiteral("Recent"),
                                          QStringLiteral("recent-user"));
    auto first = makeReplyHistoryMessage(QStringLiteral("first-outgoing"),
                                         QStringLiteral("Me"),
                                         QStringLiteral("self-id"));
    auto second = makeReplyHistoryMessage(QStringLiteral("second-outgoing"),
                                          QStringLiteral("Me"),
                                          QStringLiteral("self-id"));
    first->replyParent = recent;
    second->replyParent = recent;
    this->addMessage(older);
    this->addMessage(first);
    this->addMessage(second);

    sendKey(*this->editor, Qt::Key_Backspace);
    ASSERT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Recent "));

    sendKey(*this->editor, Qt::Key_Backspace);
    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Older "));
}

TEST_F(ReplyHistorySelectionTest, BackspacePreservesNoTargetBehavior)
{
    this->addMessage(makeReplyHistoryMessage(QStringLiteral("plain-id"),
                                              QStringLiteral("Alice"),
                                              QStringLiteral("alice-id")));

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_TRUE(this->split->getInput().getInputText().isEmpty());
}

TEST_F(ReplyHistorySelectionTest, BackspaceEditsNonEmptyInputNormally)
{
    this->split->getInput().setInputText(QStringLiteral("draft"));
    this->editor->setFocus();
    this->editor->moveCursor(QTextCursor::End);

    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(), QStringLiteral("draf"));
}

TEST_F(MultiChannelSplitInputTest,
       ReplyHistoryKeepsCollidingIDsChannelScopedWhileCycling)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.accountID = QStringLiteral("self-alpha");
    beta->context.accountID = QStringLiteral("self-beta");

    auto alphaTarget = makeReplyHistoryMessage(
        QStringLiteral("same-id"), QStringLiteral("AlphaTarget"),
        QStringLiteral("alpha-user"), true, QStringLiteral("alpha"));
    auto alphaOutgoing = makeReplyHistoryMessage(
        QStringLiteral("alpha-outgoing"), QStringLiteral("AlphaMe"),
        QStringLiteral("self-alpha"), false, QStringLiteral("alpha"));
    alphaOutgoing->replyParent = alphaTarget;

    auto betaTarget = makeReplyHistoryMessage(
        QStringLiteral("same-id"), QStringLiteral("BetaTarget"),
        QStringLiteral("beta-user"), false, QStringLiteral("beta"));
    auto betaOutgoing = makeReplyHistoryMessage(
        QStringLiteral("beta-outgoing"), QStringLiteral("BetaMe"),
        QStringLiteral("self-beta"), false, QStringLiteral("beta"));
    betaOutgoing->replyParent = betaTarget;

    auto mismatchedParent = makeReplyHistoryMessage(
        QStringLiteral("mismatched-parent"), QStringLiteral("WrongParent"),
        QStringLiteral("wrong-user"), false, QStringLiteral("beta"));
    auto mismatchedOutgoing = makeReplyHistoryMessage(
        QStringLiteral("mismatched-outgoing"), QStringLiteral("AlphaMe"),
        QStringLiteral("self-alpha"), false, QStringLiteral("alpha"));
    mismatchedOutgoing->replyParent = mismatchedParent;

    alpha->addMessage(alphaTarget, MessageContext::Original);
    alpha->addMessage(alphaOutgoing, MessageContext::Original);
    alpha->addMessage(mismatchedOutgoing, MessageContext::Original);
    beta->addMessage(betaTarget, MessageContext::Original);
    beta->addMessage(betaOutgoing, MessageContext::Original);
    QCoreApplication::processEvents();

    ASSERT_EQ(this->multi->getMessageSnapshot().size(), 5U);
    ASSERT_EQ(this->split->getChannelView().inferChannel(*alphaTarget),
              alpha);
    ASSERT_EQ(this->split->getChannelView().inferChannel(*betaTarget), beta);

    ASSERT_TRUE(this->input.replyToRecentMentionOrCycle(100, false));
    ASSERT_EQ(this->input.replyTarget(), betaTarget);
    ASSERT_EQ(this->input.replyChannel(), beta);

    ASSERT_TRUE(this->input.replyToRecentMentionOrCycle(100, false));
    EXPECT_EQ(this->input.replyTarget(), alphaTarget);
    EXPECT_EQ(this->input.replyChannel(), alpha);
}

TEST_F(MessageHistorySelectionTest, PlainKMovesToOlderMessageWhileActive)
{
    this->addMessage(QStringLiteral("older message"));
    this->addMessage(QStringLiteral("newest message"));

    this->activateSelection();
    sendKey(*this->editor, Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));

    EXPECT_TRUE(this->split->getInput().getInputText().isEmpty());
    this->copySelectionToInput();
    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("older message"));
}

TEST_F(MessageHistorySelectionTest, PlainJMovesBackToNewerMessageWhileActive)
{
    this->addMessage(QStringLiteral("older message"));
    this->addMessage(QStringLiteral("newest message"));

    this->activateSelection();
    sendKey(*this->editor, Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    sendKey(*this->editor, Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));

    EXPECT_TRUE(this->split->getInput().getInputText().isEmpty());
    this->copySelectionToInput();
    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("newest message"));
}

TEST_F(MessageHistorySelectionTest, PlainJAndKTypeNormallyWhileInactive)
{
    sendKey(*this->editor, Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    sendKey(*this->editor, Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));

    EXPECT_EQ(this->split->getInput().getInputText(), QStringLiteral("jk"));
}

TEST_F(MessageHistorySelectionTest, EscapeRestoresNormalJAndKTyping)
{
    this->addMessage(QStringLiteral("message"));

    this->activateSelection();
    sendKey(*this->editor, Qt::Key_Escape);
    sendKey(*this->editor, Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    sendKey(*this->editor, Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));

    EXPECT_EQ(this->split->getInput().getInputText(), QStringLiteral("jk"));
}

TEST_F(MessageHistorySelectionTest,
       RumbleBackspaceCreatesLoggedOutDraftWithoutDiscardingInput)
{
    auto rumble = std::make_shared<Channel>(QStringLiteral("rumble"),
                                            Channel::Type::Rumble);
    this->setChannel(rumble);
    this->addMessage(makeRumbleHistorySelectionMessage(
        QStringLiteral("rumble message"), QStringLiteral("rumble")));
    this->split->getInput().setInputText(QStringLiteral("draft body"));

    EXPECT_FALSE(rumble->canSendMessage());

    this->activateSelection();
    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Viewer draft body "));
    EXPECT_FALSE(this->split->getChannelView().paused());

    // A second Escape is now handled by SplitInput, proving Backspace created
    // a structured reply binding rather than only inserting visible text.
    sendKey(*this->editor, Qt::Key_Escape);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("draft body"));
}

TEST_F(MessageHistorySelectionTest,
       RumbleBackspaceTargetsRumbleChildInMultiChannel)
{
    auto twitch = std::make_shared<Channel>(QStringLiteral("alpha"),
                                            Channel::Type::Twitch);
    auto rumble = std::make_shared<Channel>(QStringLiteral("rumble"),
                                            Channel::Type::Rumble);
    const std::array specs{
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Twitch,
            .name = QStringLiteral("alpha"),
        },
        MultiChannel::Spec{
            .platform = MultiChannel::Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/c/rumble"),
            .layoutIdentity =
                ChannelLayoutIdentity{
                    .platform = QStringLiteral("rumble"),
                    .locator = QStringLiteral("https://rumble.com/c/rumble"),
                },
        },
    };
    const std::array<ChannelPtr, 2> children{twitch, rumble};
    auto next = std::make_shared<size_t>(0);
    auto multi = std::make_shared<MultiChannel>(
        specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [children, next](const MultiChannel::Spec &) mutable -> ChannelPtr {
            return children[(*next)++];
        });
    multi->setActiveChannelIndex(1);
    this->setChannel(multi);

    rumble->addMessage(
        makeRumbleHistorySelectionMessage(
            QStringLiteral("multi rumble message"), QStringLiteral("rumble")),
        MessageContext::Original);
    QCoreApplication::processEvents();

    this->activateSelection();
    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_EQ(this->split->getInput().getInputText(),
              QStringLiteral("@Viewer "));
    EXPECT_FALSE(this->split->getChannelView().paused());
}

TEST_F(MessageHistorySelectionTest, RumbleBackspaceReportsNonReplyableSelection)
{
    auto rumble = std::make_shared<Channel>(QStringLiteral("rumble"),
                                            Channel::Type::Rumble);
    this->setChannel(rumble);
    this->addMessage(QStringLiteral("message without a reply identity"));

    this->activateSelection();
    sendKey(*this->editor, Qt::Key_Backspace);

    EXPECT_TRUE(this->split->getInput().getInputText().isEmpty());
    EXPECT_FALSE(this->split->getChannelView().paused());
    const auto messages = rumble->getMessageSnapshot();
    ASSERT_GE(messages.size(), 2U);
    EXPECT_TRUE(messages.back()->messageText.contains(
        QStringLiteral("cannot be used as a reply")));
}

TEST_F(MessageHistorySelectionTest,
       EscapeImmediatelyRestoresSnapshotPauseAndPriorScrollState)
{
    auto rumble = std::make_shared<Channel>(QStringLiteral("rumble"),
                                            Channel::Type::Rumble);
    this->setChannel(rumble);
    this->addMessage(makeRumbleHistorySelectionMessage(
        QStringLiteral("older rumble message"), QStringLiteral("rumble")));
    this->addMessage(makeRumbleHistorySelectionMessage(
        QStringLiteral("newer rumble message"), QStringLiteral("rumble")));

    this->split->show();
    this->editor->setFocus();
    QCoreApplication::processEvents();

    auto &view = this->split->getChannelView();
    auto &scrollbar = view.getScrollBar();
    view.setEnableScrollingToBottom(false);
    scrollbar.setMinimum(5);
    scrollbar.setMaximum(25);
    scrollbar.setPageSize(5);
    scrollbar.setDesiredValue(8, false);
    const auto relativeScrollBefore =
        scrollbar.getDesiredValue() - scrollbar.getMinimum();
    const auto snapshotSize = view.getMessagesSnapshot().size();
    ASSERT_EQ(snapshotSize, 2U);
    ASSERT_DOUBLE_EQ(scrollbar.getDesiredValue() - scrollbar.getMinimum(),
                     relativeScrollBefore);

    this->activateSelection();
    EXPECT_TRUE(view.paused());

    sendKey(*this->editor, Qt::Key_Escape);

    EXPECT_FALSE(view.paused());
    EXPECT_FALSE(view.getEnableScrollingToBottom());
    EXPECT_DOUBLE_EQ(scrollbar.getDesiredValue() - scrollbar.getMinimum(),
                     relativeScrollBefore);
    EXPECT_EQ(view.getMessagesSnapshot().size(), snapshotSize);
    EXPECT_EQ(QApplication::focusWidget(), this->editor);
}
