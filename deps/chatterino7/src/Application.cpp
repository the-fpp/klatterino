// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"

#include "common/Args.hpp"
#include "common/Channel.hpp"
#include "common/Modes.hpp"
#include "common/Version.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/ignores/IgnoreController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/pronouns/Pronouns.hpp"
#include "providers/rumble/RumbleApplicationController.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/eventsub/Controller.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/NativeMessaging.hpp"
#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/PluginController.hpp"
#endif
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/sound/MiniaudioBackend.hpp"
#include "controllers/sound/NullBackend.hpp"
#include "controllers/twitch/LiveController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/chatterino/ChatterinoBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/PubSubMessages.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/helper/LoggingChannel.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/Updates.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/MultiChannel.hpp"
#include "util/PostToThread.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/dialogs/SelectChannelDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/Window.hpp"

#include <miniaudio.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <array>
#include <span>

namespace {

using namespace chatterino;

const QString BTTV_LIVE_UPDATES_URL = "wss://sockets.betterttv.net/ws";
const QString SEVENTV_EVENTAPI_URL = "wss://events.7tv.io/v3";

std::atomic<bool> STOPPED{false};
std::atomic<bool> ABOUT_TO_QUIT{false};

void installRumbleUsercardTestFixture()
{
    if (qEnvironmentVariable("CHATTERINO_UI_TEST_RUMBLE_USERCARD_FIXTURE") !=
        QStringLiteral("1"))
    {
        return;
    }

    QTimer::singleShot(250, [] {
        if (auto *paints = getApp()->getSeventvPaints())
        {
            paints->addPaint(QJsonObject{
                {QStringLiteral("id"),
                 QStringLiteral("rumble-usercard-fixture-paint")},
                {QStringLiteral("name"), QStringLiteral("Fixture paint")},
                {QStringLiteral("function"), QStringLiteral("LINEAR_GRADIENT")},
                {QStringLiteral("repeat"), false},
                {QStringLiteral("angle"), 0},
                {QStringLiteral("stops"),
                 QJsonArray{
                     QJsonObject{{QStringLiteral("at"), 0.0},
                                 {QStringLiteral("color"), 0x0000FFFF}},
                     QJsonObject{{QStringLiteral("at"), 1.0},
                                 {QStringLiteral("color"), 0x0000FFFF}},
                 }},
                {QStringLiteral("shadows"), QJsonArray{}},
            });
            const std::array<seventv::eventapi::User, 1> users{
                seventv::eventapi::TwitchUser(QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("fixture-twitch")},
                    {QStringLiteral("username"),
                     QStringLiteral("synthetic-rumble")},
                }),
            };
            paints->assignPaintToUsers(
                QStringLiteral("rumble-usercard-fixture-paint"), users);
        }

        auto *container = getApp()
                              ->getWindows()
                              ->getMainWindow()
                              .getNotebook()
                              .getOrAddSelectedPage();
        auto *split = container->getSelectedSplit();
        if (!split)
        {
            split = container->appendNewSplit(false);
        }

        auto channel = std::make_shared<Channel>(
            QStringLiteral("rumble-usercard-fixture"), Channel::Type::Misc);
        split->setChannel(channel);

        MessageBuilder builder;
        builder->id = QStringLiteral("fixture-rumble-message");
        builder->userID = QStringLiteral("fixture-rumble-user");
        builder->loginName = QStringLiteral("synthetic-rumble");
        builder->displayName = QStringLiteral("Synthetic Rumble");
        builder->channelName = channel->getName();
        builder->usernameColor = QColor(QStringLiteral("#4CB050"));
        builder->platform = MessagePlatform::Rumble;
        builder->messageText = QStringLiteral("usercard isolation fixture");
        builder->rumble = RumbleMessageMetadata{
            .channelID = QStringLiteral("fixture-rumble-channel"),
            .badgeIDs = {QStringLiteral("verified"),
                         QStringLiteral("recurring_subscription")},
            .roleIDs = {QStringLiteral("moderator")},
            .source = QStringLiteral("fixture"),
        };
        builder.emplace<TimestampElement>(QTime::currentTime());
        builder
            .emplace<TextElement>(
                QStringLiteral("Synthetic Rumble:"),
                MessageElementFlags{MessageElementFlag::Username,
                                    MessageElementFlag::RumbleUsername},
                builder->usernameColor, FontStyle::ChatMediumBold)
            ->setLink({Link::UserInfo, QStringLiteral("synthetic-rumble")});
        builder.emplace<TextElement>(
            QStringLiteral("usercard isolation fixture"),
            MessageElementFlag::Text, MessageColor::Text);
        channel->addMessage(builder.release(), MessageContext::Original);
    });
}

class RumbleReplyTestChannel final : public Channel
{
public:
    explicit RumbleReplyTestChannel(Split *split)
        : Channel(QStringLiteral("rumble-reply-fixture"), Type::Rumble)
        , split_(split)
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

    void setExpectedMessage(QString message)
    {
        this->expectedMessage_ = std::move(message);
    }

    void sendMessageAsync(QString message, SendCallback callback) override
    {
        const auto exact = message == this->expectedMessage_;
        const auto sendCount = ++this->sendCount_;
        const QPointer<Split> split = this->split_;
        const auto weak = this->weak_from_this();

        if (callback)
        {
            callback({SendOutcome::Confirmed, {}});
        }

        QTimer::singleShot(0, [weak, split, exact, sendCount] {
            const auto channel = weak.lock();
            if (!channel)
            {
                return;
            }
            const bool inputEmpty =
                split && split->getInput().getInputText().isEmpty();
            channel->addSystemMessage(
                QStringLiteral(
                    "Rumble fallback sends %1 exact %2 input empty %3")
                    .arg(sendCount)
                    .arg(exact ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(inputEmpty ? QStringLiteral("yes")
                                    : QStringLiteral("no")));
        });
    }

private:
    QPointer<Split> split_;
    QString expectedMessage_;
    size_t sendCount_ = 0;
};

void installRumbleReplyTestFixture()
{
    if (qEnvironmentVariable("CHATTERINO_UI_TEST_RUMBLE_REPLY_FIXTURE") !=
        QStringLiteral("1"))
    {
        return;
    }

    QTimer::singleShot(250, [] {
        auto *container = getApp()
                              ->getWindows()
                              ->getMainWindow()
                              .getNotebook()
                              .getOrAddSelectedPage();
        auto *split = container->getSelectedSplit();
        if (!split)
        {
            split = container->appendNewSplit(false);
        }

        auto channel = std::make_shared<RumbleReplyTestChannel>(split);
        split->setChannel(IndirectChannel{channel});

        MessageBuilder builder;
        builder->id = QStringLiteral("fixture-rumble-reply");
        builder->userID = QStringLiteral("fixture-rumble-author");
        builder->loginName = QStringLiteral("fixtureauthor");
        builder->displayName = QStringLiteral("FixtureAuthor");
        builder->channelName = channel->getName();
        builder->platform = MessagePlatform::Rumble;
        builder->messageText = QStringLiteral("synthetic reply source");
        builder.emplace<TimestampElement>(QTime::currentTime());
        builder.emplace<TextElement>(
            QStringLiteral("FixtureAuthor:"),
            MessageElementFlags{MessageElementFlag::Username,
                                MessageElementFlag::RumbleUsername},
            MessageColor::Text, FontStyle::ChatMediumBold);
        builder.emplace<TextElement>(QStringLiteral("synthetic reply source"),
                                     MessageElementFlag::Text,
                                     MessageColor::Text);
        const auto reply = builder.release();
        channel->addMessage(reply, MessageContext::Original);

        split->getInput().setInputText(
            QStringLiteral("ordinary fallback body"));
        split->setInputReply(reply, channel);
        channel->setExpectedMessage(split->getInput().getInputText());
    });
}

class RumbleLifecycleTestFixture final : public QObject
{
public:
    RumbleLifecycleTestFixture()
        : QObject(qApp)
        , dispatcher_(makeQtRumbleDispatcher(this))
        , provider_(std::make_unique<RumbleChannelProvider>(dispatcher_))
    {
    }

    void install()
    {
        auto created =
            provider_->getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                   QStringLiteral("lifecycle-fixture"));
        if (!created)
        {
            return;
        }
        channel_ = *created;
        auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
        container_ = notebook.getOrAddSelectedPage();
        split_ = container_->getSelectedSplit();
        if (!split_)
        {
            split_ = container_->appendNewSplit(false);
        }
        split_->setChannel(IndirectChannel{
            channel_, Channel::Type::Rumble,
            ChannelLayoutIdentity{
                .platform = QStringLiteral("rumble"),
                .locator =
                    QStringLiteral("https://rumble.com/c/lifecycle-fixture"),
            }});
        getSettings()->headerStreamTitle.setValue(true);
        auto operation =
            channel_->beginOperation(RumbleOperationKind::Resolver);
        if (!operation)
        {
            return;
        }
        operation_ = *operation;
        transitionsOk_ &=
            this->publishTitle(QStringLiteral("Initial Stream Title"));
        split_->getInput().setInputText(QStringLiteral("preserved draft"));
        displayName_ = channel_->getDisplayName();
        if (auto *tab = container_->getTab())
        {
            tabTitle_ = tab->getTitle();
        }

        transitionsOk_ &= this->transition(RumbleChannelState::Connecting);
        transitionsOk_ &= this->transition(RumbleChannelState::Connected);
        QTimer::singleShot(100, this, [this] {
            liveOk_ = this->stateMatches(true);
            initialTitleOk_ = this->headerMatches(
                QStringLiteral("lifecycle-fixture (live) - Initial Stream "
                               "Title"));
            tabStableOk_ &= this->tabMatches();
            transitionsOk_ &=
                this->publishTitle(QStringLiteral("Changed Stream Title"));
            changedTitleOk_ = this->headerMatches(
                QStringLiteral("lifecycle-fixture (live) - Changed Stream "
                               "Title"));
            tabStableOk_ &= this->tabMatches();
            transitionsOk_ &= this->transition(RumbleChannelState::Backoff);
            QTimer::singleShot(100, this, [this] {
                backoffOk_ = this->stateMatches(true) &&
                             this->headerMatches(QStringLiteral(
                                 "lifecycle-fixture (live) - Changed Stream "
                                 "Title"));
                transitionsOk_ &=
                    this->transition(RumbleChannelState::Unresolved);
                clearedTitleOk_ = channel_->streamTitle().isEmpty() &&
                                  this->headerMatches(QStringLiteral(
                                      "lifecycle-fixture (live)"));
                transitionsOk_ &= this->transition(RumbleChannelState::Offline);
                QTimer::singleShot(100, this, [this] {
                    offlineOk_ = this->stateMatches(false) &&
                                 this->headerMatches(
                                     QStringLiteral("lifecycle-fixture"));
                    tabStableOk_ &= this->tabMatches();
                    transitionsOk_ &=
                        this->transition(RumbleChannelState::Unresolved);
                    transitionsOk_ &= this->publishTitle(
                        QStringLiteral("Restored Stream Title"));
                    transitionsOk_ &=
                        this->transition(RumbleChannelState::Connecting);
                    transitionsOk_ &=
                        this->transition(RumbleChannelState::Connected);
                    QTimer::singleShot(100, this, [this] {
                        restoredOk_ =
                            this->stateMatches(true) &&
                            this->headerMatches(QStringLiteral(
                                "lifecycle-fixture (live) - Restored Stream "
                                "Title"));
                        tabStableOk_ &= this->tabMatches();
                        draftOk_ =
                            split_ && split_->getInput().getInputText() ==
                                          QStringLiteral("preserved draft");
                        identityOk_ =
                            split_ &&
                            split_->getSelectedLocator() ==
                                QStringLiteral("https://rumble.com/c/"
                                               "lifecycle-fixture") &&
                            channel_->getDisplayName() == displayName_;
                        channel_->addSystemMessage(
                            QStringLiteral(
                                "Rumble lifecycle live %1 backoff %2 offline "
                                "%3 restored %4 draft %5 identity %6 "
                                "transitions %7 titles %8 tabstable %9")
                                .arg(this->yesNo(liveOk_))
                                .arg(this->yesNo(backoffOk_))
                                .arg(this->yesNo(offlineOk_))
                                .arg(this->yesNo(restoredOk_))
                                .arg(this->yesNo(draftOk_))
                                .arg(this->yesNo(identityOk_))
                                .arg(this->yesNo(transitionsOk_))
                                .arg(this->yesNo(
                                    initialTitleOk_ && changedTitleOk_ &&
                                    clearedTitleOk_ && restoredOk_))
                                .arg(this->yesNo(tabStableOk_)));
                        // Retire the fixture while the application singleton
                        // is still alive. QApplication destroys its children
                        // after Chatterino's normal shutdown has completed.
                        this->deleteLater();
                    });
                });
            });
        });
    }

private:
    bool transition(RumbleChannelState state)
    {
        return channel_ && static_cast<bool>(channel_->transitionTo(state));
    }

    bool publishTitle(QString title)
    {
        if (!channel_ || !operation_)
        {
            return false;
        }
        const auto expected = title;
        auto metadata = RumbleResolvedMetadata::create(
            QStringLiteral("lifecycle-fixture"), std::nullopt, std::nullopt,
            std::nullopt, std::move(title));
        if (!metadata)
        {
            return false;
        }
        channel_->publishMetadata(*operation_, std::move(*metadata));
        return channel_->streamTitle() == expected;
    }

    bool headerMatches(const QString &expected) const
    {
        if (!split_)
        {
            return false;
        }
        auto *header = split_->findChild<SplitHeader *>();
        return header && header->channelText() == expected;
    }

    bool tabMatches() const
    {
        if (!container_ || tabTitle_.isEmpty())
        {
            return false;
        }
        auto *tab = container_->getTab();
        return tab && tab->getTitle() == tabTitle_ &&
               !tab->getTitle().contains(QStringLiteral("Stream Title"));
    }

    bool stateMatches(bool live) const
    {
        if (!channel_ || !container_)
        {
            return false;
        }
        auto *tab = container_->getTab();
        return tab && channel_->isLive() == live && tab->isLive() == live;
    }

    static QString yesNo(bool value)
    {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    }

    std::shared_ptr<RumbleDispatcher> dispatcher_;
    std::unique_ptr<RumbleChannelProvider> provider_;
    std::shared_ptr<RumbleChannel> channel_;
    std::optional<RumbleOperationToken> operation_;
    QPointer<SplitContainer> container_;
    QPointer<Split> split_;
    QString displayName_;
    QString tabTitle_;
    bool transitionsOk_ = true;
    bool liveOk_ = false;
    bool backoffOk_ = false;
    bool offlineOk_ = false;
    bool restoredOk_ = false;
    bool draftOk_ = false;
    bool identityOk_ = false;
    bool initialTitleOk_ = false;
    bool changedTitleOk_ = false;
    bool clearedTitleOk_ = false;
    bool tabStableOk_ = true;
};

void installRumbleLifecycleTestFixture()
{
    if (qEnvironmentVariable("CHATTERINO_UI_TEST_RUMBLE_LIFECYCLE_FIXTURE") !=
        QStringLiteral("1"))
    {
        return;
    }

    auto *fixture = new RumbleLifecycleTestFixture;
    QTimer::singleShot(250, fixture, [fixture] {
        fixture->install();
    });
}

class AutomaticRoutingTestChannel final : public Channel
{
public:
    AutomaticRoutingTestChannel(QString name, Type type, QString platform,
                                bool live, bool writable)
        : Channel(std::move(name), type)
        , live_(live)
    {
        context_.platform = std::move(platform);
        context_.channelID = getName();
        context_.accountID = context_.platform + QStringLiteral("-fixture");
        context_.authenticated = true;
        context_.writable = writable;
    }

    bool canSendMessage() const override
    {
        return context_.authenticated && context_.writable;
    }

    bool isWritable() const override
    {
        return context_.writable;
    }

    bool isLive() const override
    {
        return live_;
    }

    MessageSendContext messageSendContext() const override
    {
        return context_;
    }

    void setAvailable(bool live, bool writable)
    {
        live_ = live;
        context_.writable = writable;
    }

    void expect(QString message)
    {
        expected_ = std::move(message);
    }

    void sendMessageAsync(QString message, SendCallback callback) override
    {
        ++sendCount_;
        allExact_ &= message == expected_;
        if (callback)
        {
            callback({SendOutcome::Confirmed, {}});
        }
    }

    size_t sendCount() const
    {
        return sendCount_;
    }

    bool allExact() const
    {
        return allExact_;
    }

private:
    MessageSendContext context_;
    QString expected_;
    bool live_ = false;
    bool allExact_ = true;
    size_t sendCount_ = 0;
};

class AutomaticRoutingTestFixture final : public QObject
{
public:
    AutomaticRoutingTestFixture()
        : QObject(qApp)
    {
    }

    void install()
    {
        rumble_ = std::make_shared<AutomaticRoutingTestChannel>(
            QStringLiteral("routing-fixture-rumble"), Channel::Type::Rumble,
            QStringLiteral("rumble"), false, false);
        twitch_ = std::make_shared<AutomaticRoutingTestChannel>(
            QStringLiteral("routing-fixture-twitch"), Channel::Type::Twitch,
            QStringLiteral("twitch"), false, true);
        const std::array specs{
            MultiChannel::Spec{
                .platform = MultiChannel::Platform::Rumble,
                .name = QStringLiteral("https://rumble.com/c/routing-fixture")},
            MultiChannel::Spec{
                .platform = MultiChannel::Platform::Twitch,
                .name = QStringLiteral("routing-fixture-twitch")},
        };
        multi_ = std::make_shared<MultiChannel>(
            std::span{specs},
            MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
            [this](const MultiChannel::Spec &spec) -> ChannelPtr {
                return spec.platform == MultiChannel::Platform::Rumble
                           ? rumble_
                           : twitch_;
            });

        auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
        container_ = notebook.getOrAddSelectedPage();
        split_ = container_->getSelectedSplit();
        if (!split_)
        {
            split_ = container_->appendNewSplit(false);
        }
        split_->setChannel(IndirectChannel{multi_});
        for (auto *widget : split_->getInput().findChildren<QWidget *>())
        {
            if (auto *editor = dynamic_cast<ResizingTextEdit *>(widget))
            {
                editor_ = editor;
                break;
            }
        }
        button_ = dynamic_cast<PixmapButton *>(
            split_->getInput().findChild<QWidget *>(
                QStringLiteral("multiChannelRoutingPlatformButton")));
        indicator_ = split_->getInput().findChild<QWidget *>(
            QStringLiteral("multiChannelAutomaticRoutingIndicator"));

        firstDraft_ = QStringLiteral("offline fallback fixture");
        twitch_->expect(firstDraft_);
        split_->getInput().setInputText(firstDraft_);
        QTimer::singleShot(150, this, [this] {
            offlinePreviewOk_ =
                this->previewMatches(twitch_, "Twitch", true);
            firstDraftOk_ = this->draftMatches(firstDraft_);
            this->sendEnter();
            QTimer::singleShot(150, this, [this] {
                firstSendOk_ = twitch_->sendCount() == 1 &&
                               rumble_->sendCount() == 0 &&
                               twitch_->allExact() && split_ &&
                               split_->getInput().getInputText().isEmpty();
                const auto feedback = multi_->getLastMessage();
                noticeAbsentOk_ =
                    feedback == nullptr ||
                    !feedback->messageText.contains(QStringLiteral("Sent via"));

                secondDraft_ = QStringLiteral("restored primary fixture");
                rumble_->expect(secondDraft_);
                split_->getInput().setInputText(secondDraft_);
                rumble_->setAvailable(true, true);
                multi_->childStateChanged.invoke();
                QTimer::singleShot(150, this, [this] {
                    restoredPreviewOk_ =
                        this->previewMatches(rumble_, "Rumble", false);
                    secondDraftOk_ = this->draftMatches(secondDraft_);
                    this->sendEnter();
                    QTimer::singleShot(150, this, [this] {
                        secondSendOk_ =
                            rumble_->sendCount() == 1 &&
                            twitch_->sendCount() == 1 && rumble_->allExact() &&
                            split_ &&
                            split_->getInput().getInputText().isEmpty();
                        layoutOk_ = multi_->activeChannelIndex() == 0 &&
                            multi_->channels().size() == 2 &&
                            multi_->channels()[0].channel == rumble_ &&
                            multi_->channels()[1].channel == twitch_;
                        visualDraft_ =
                            QStringLiteral("visible fallback indicator");
                        split_->getInput().setInputText(visualDraft_);
                        rumble_->setAvailable(false, false);
                        multi_->childStateChanged.invoke();
                        QTimer::singleShot(150, this, [this] {
                            visualGlyphOk_ = this->previewMatches(
                                twitch_, "Twitch", true);
                            this->showResult();
                            this->deleteLater();
                        });
                    });
                });
            });
        });
    }

private:
    bool previewMatches(const ChannelPtr &expected,
                        const QString &platformName,
                        bool expectAutomaticIndicator) const
    {
        if (!multi_ || !button_ || !indicator_ || !split_)
        {
            return false;
        }
        const auto text = split_->getInput().getInputText();
        const auto preview = multi_->previewMessageDraftDestination(
            MessageDraft::fromPlainText(text), text,
            multi_->activeChannelIndex());
        const auto indicatorVisible = !indicator_->isHidden();
        const auto indicatorTextOk =
            !expectAutomaticIndicator ||
            (indicator_->toolTip().contains(platformName) &&
             indicator_->toolTip().contains(
                 QStringLiteral("best available destination")) &&
             indicator_->accessibleName() == indicator_->toolTip());
        return preview.destination == expected &&
               button_->toolTip().contains(platformName) &&
               indicatorVisible == expectAutomaticIndicator &&
               indicatorTextOk;
    }

    void showResult()
    {
        const auto details =
            QStringLiteral(
                "automatic-routing offline=%1 first-send=%2 first-draft=%3 "
                "restored=%4 second-send=%5 second-draft=%6 layout=%7 "
                "notice-absent=%8 glyph=%9")
                .arg(this->yesNo(offlinePreviewOk_))
                .arg(this->yesNo(firstSendOk_))
                .arg(this->yesNo(firstDraftOk_))
                .arg(this->yesNo(restoredPreviewOk_))
                .arg(this->yesNo(secondSendOk_))
                .arg(this->yesNo(secondDraftOk_))
                .arg(this->yesNo(layoutOk_))
                .arg(this->yesNo(noticeAbsentOk_))
                .arg(this->yesNo(visualGlyphOk_));
        getApp()->getWindows()->getMainWindow().setWindowTitle(
            QStringLiteral("Chatterino %1").arg(details));
        multi_->addSystemMessage(
            QStringLiteral(
                "Automatic routing offline fallback %1 first send once %2 "
                "first draft %3 restored primary %4 second send once %5 "
                "second draft %6 layout stable %7 notice absent %8 routing "
                "glyph %9")
                .arg(this->yesNo(offlinePreviewOk_))
                .arg(this->yesNo(firstSendOk_))
                .arg(this->yesNo(firstDraftOk_))
                .arg(this->yesNo(restoredPreviewOk_))
                .arg(this->yesNo(secondSendOk_))
                .arg(this->yesNo(secondDraftOk_))
                .arg(this->yesNo(layoutOk_))
                .arg(this->yesNo(noticeAbsentOk_))
                .arg(this->yesNo(visualGlyphOk_)));
    }

    bool draftMatches(const QString &expected) const
    {
        return split_ && split_->getInput().getInputText() == expected;
    }

    void sendEnter()
    {
        if (!editor_)
        {
            return;
        }
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor_, &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor_, &release);
    }

    static QString yesNo(bool value)
    {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    }

    std::shared_ptr<AutomaticRoutingTestChannel> rumble_;
    std::shared_ptr<AutomaticRoutingTestChannel> twitch_;
    std::shared_ptr<MultiChannel> multi_;
    QPointer<SplitContainer> container_;
    QPointer<Split> split_;
    QPointer<ResizingTextEdit> editor_;
    QPointer<PixmapButton> button_;
    QPointer<QWidget> indicator_;
    QString firstDraft_;
    QString secondDraft_;
    QString visualDraft_;
    bool offlinePreviewOk_ = false;
    bool firstSendOk_ = false;
    bool firstDraftOk_ = false;
    bool restoredPreviewOk_ = false;
    bool secondSendOk_ = false;
    bool secondDraftOk_ = false;
    bool layoutOk_ = false;
    bool noticeAbsentOk_ = false;
    bool visualGlyphOk_ = false;
};

void installAutomaticRoutingTestFixture()
{
    if (qEnvironmentVariable("CHATTERINO_UI_TEST_AUTOMATIC_ROUTING_FIXTURE") !=
        QStringLiteral("1"))
    {
        return;
    }

    auto *fixture = new AutomaticRoutingTestFixture;
    QTimer::singleShot(250, fixture, [fixture] {
        fixture->install();
    });
}

class CompactTabRevealTestFixture final : public QObject
{
public:
    CompactTabRevealTestFixture()
        : QObject(qApp)
    {
    }

    void install()
    {
        window_ = &getApp()->getWindows()->getMainWindow();
        notebook_ = &window_->getNotebook();
        anchor_ = notebook_->getOrAddSelectedPage();
        preexistingEmpty_ = notebook_->addPage(false);
        notebook_->select(anchor_);
        notebook_->setCompactMode(true);
        notebook_->setCompactTabsRevealed(true);

        QTimer::singleShot(100, this, [this] {
            this->click(preexistingEmpty_->getTab());
            tabClickOk_ = notebook_->getSelectedPage() == preexistingEmpty_ &&
                          notebook_->areCompactTabsRevealed();

            notebook_->select(anchor_);
            beforeAddCount_ = notebook_->getVisibleTabCount();
            auto *addButton = notebook_->findChild<QWidget *>(
                QStringLiteral("notebookAddTab"));
            addAccessibleOk_ =
                addButton && addButton->accessibleName() ==
                                 QStringLiteral("Add tab");
            this->click(addButton);

            QTimer::singleShot(200, this, [this] {
                added_ = notebook_->getSelectedPage();
                addOnceOk_ =
                    added_ && added_ != preexistingEmpty_ &&
                    notebook_->getVisibleTabCount() == beforeAddCount_ + 1 &&
                    notebook_->areCompactTabsRevealed();
                for (auto *widget : qApp->topLevelWidgets())
                {
                    auto *candidate =
                        dynamic_cast<SelectChannelDialog *>(widget);
                    if (candidate && candidate->isVisible())
                    {
                        picker_ = candidate;
                        break;
                    }
                }
                pickerOpenOk_ = picker_ && picker_->isVisible() &&
                                notebook_->areCompactTabsRevealed();
                if (picker_)
                {
                    picker_->close();
                }

                QTimer::singleShot(200, this, [this] {
                    pickerClosedOk_ = picker_.isNull() && added_ &&
                                      notebook_->getSelectedPage() == added_ &&
                                      notebook_->areCompactTabsRevealed();
                    this->click(window_);
                    outsideDismissOk_ =
                        !notebook_->areCompactTabsRevealed();
                    this->showResult();
                    this->deleteLater();
                });
            });
        });
    }

private:
    static void click(QWidget *widget)
    {
        if (!widget)
        {
            return;
        }
        const auto local = QPointF(widget->rect().center());
        const auto global = QPointF(widget->mapToGlobal(local.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &release);
    }

    void showResult()
    {
        if (!window_)
        {
            return;
        }
        const auto details =
            QStringLiteral(
                "compact-reveal tab=%1 add=%2 picker-open=%3 "
                "picker-closed=%4 empty=%5 outside=%6 accessible=%7")
                .arg(this->yesNo(tabClickOk_))
                .arg(this->yesNo(addOnceOk_))
                .arg(this->yesNo(pickerOpenOk_))
                .arg(this->yesNo(pickerClosedOk_))
                .arg(this->yesNo(added_ && added_ != preexistingEmpty_))
                .arg(this->yesNo(outsideDismissOk_))
                .arg(this->yesNo(addAccessibleOk_));
        window_->setWindowTitle(QStringLiteral("Chatterino %1").arg(details));
        auto *result = new QLabel(
            QStringLiteral(
                "Compact reveal tab %1 add once %2 picker open %3 picker "
                "closed %4 empty isolated %5 outside %6 accessible %7")
                .arg(this->yesNo(tabClickOk_))
                .arg(this->yesNo(addOnceOk_))
                .arg(this->yesNo(pickerOpenOk_))
                .arg(this->yesNo(pickerClosedOk_))
                .arg(this->yesNo(added_ && added_ != preexistingEmpty_))
                .arg(this->yesNo(outsideDismissOk_))
                .arg(this->yesNo(addAccessibleOk_)),
            window_);
        result->setObjectName(QStringLiteral("compactTabRevealFixtureResult"));
        result->setAlignment(Qt::AlignCenter);
        result->setStyleSheet(QStringLiteral(
            "QLabel { background: #111; color: #fff; font-size: 20px; "
            "padding: 18px; }"));
        result->setGeometry(80, 180, std::max(640, window_->width() - 160),
                            120);
        result->show();
        result->raise();
    }

    static QString yesNo(bool value)
    {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    }

    QPointer<Window> window_;
    QPointer<SplitNotebook> notebook_;
    QPointer<SplitContainer> anchor_;
    QPointer<SplitContainer> preexistingEmpty_;
    QPointer<SplitContainer> added_;
    QPointer<SelectChannelDialog> picker_;
    int beforeAddCount_ = 0;
    bool tabClickOk_ = false;
    bool addOnceOk_ = false;
    bool pickerOpenOk_ = false;
    bool pickerClosedOk_ = false;
    bool outsideDismissOk_ = false;
    bool addAccessibleOk_ = false;
};

void installCompactTabRevealTestFixture()
{
    if (qEnvironmentVariable("CHATTERINO_UI_TEST_COMPACT_TAB_REVEAL_FIXTURE") !=
        QStringLiteral("1"))
    {
        return;
    }

    auto *fixture = new CompactTabRevealTestFixture;
    QTimer::singleShot(250, fixture, [fixture] {
        fixture->install();
    });
}

ISoundController *makeSoundController(Settings &settings)
{
    SoundBackend soundBackend = settings.soundBackend;
    switch (soundBackend)
    {
        case SoundBackend::Miniaudio: {
            return new MiniaudioBackend(settings.soundMiniaudioKeepEngineAlive);
        }
        break;

        case SoundBackend::Null: {
            return new NullBackend();
        }
        break;

        default: {
            return new MiniaudioBackend(settings.soundMiniaudioKeepEngineAlive);
        }
        break;
    }
}

BttvLiveUpdates *makeBttvLiveUpdates(Settings &settings)
{
    bool enabled =
        settings.enableBTTVLiveUpdates &&
        (settings.enableBTTVChannelEmotes || settings.showBadgesBttv);

    if (enabled)
    {
        return new BttvLiveUpdates(BTTV_LIVE_UPDATES_URL);
    }

    return nullptr;
}

SeventvEventAPI *makeSeventvEventAPI(Settings &settings)
{
    bool enabled = settings.enableSevenTVEventAPI;

    if (enabled)
    {
        return new SeventvEventAPI(SEVENTV_EVENTAPI_URL %
                                   "?app=Chatterino&version=" %
                                   Version::instance().version());
    }

    return nullptr;
}

const QString TWITCH_PUBSUB_URL = "wss://pubsub-edge.twitch.tv";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
IApplication *INSTANCE = nullptr;

}  // namespace

namespace chatterino {

IApplication::IApplication()
{
    INSTANCE = this;
}

IApplication::~IApplication()
{
    INSTANCE = nullptr;
}

// this class is responsible for handling the workflow of Chatterino
// It will create the instances of the major classes, and connect their signals
// to each other

Application::Application(Settings &_settings, const Paths &paths,
                         const Args &_args, Updates &_updates)
    : paths_(paths)
    , args_(_args)
    , themes(new Theme(paths))
    , fonts(new Fonts(_settings))
    , logging(new Logging(_settings))
    , emotes(new EmoteController)
    , accounts(new AccountController)
    , eventSub(new eventsub::Controller())
    , hotkeys(new HotkeyController)
    , windows(new WindowManager(_args, paths, _settings, *this->themes,
                                *this->fonts))
    , toasts(new Toasts)
    , imageUploader(new ImageUploader)
    , seventvAPI(new SeventvAPI)
    , crashHandler(new CrashHandler(paths))

    , commands(new CommandController(paths))
    , notifications(new NotificationController)
    , highlights(new HighlightController(_settings, this->accounts.get()))
    , twitch(new TwitchIrcServer)
    , ffzBadges(new FfzBadges)
    , bttvBadges(new BttvBadges)
    , seventvBadges(new SeventvBadges)
    , seventvPaints(new SeventvPaints)
    , seventvPersonalEmotes(new SeventvPersonalEmotes)
    , userData(new UserDataController(paths))
    , sound(makeSoundController(_settings))
    , twitchLiveController(new TwitchLiveController)
    , twitchPubSub(new PubSub(TWITCH_PUBSUB_URL))
    , twitchBadges(new TwitchBadges)
    , chatterinoBadges(new ChatterinoBadges)
    , bttvEmotes(new BttvEmotes)
    , bttvLiveUpdates(makeBttvLiveUpdates(_settings))
    , ffzEmotes(new FfzEmotes)
    , seventvEmotes(new SeventvEmotes)
    , seventvEventAPI(makeSeventvEventAPI(_settings))
    , linkResolver(new LinkResolver)
    , streamerMode(new StreamerMode)
    , twitchUsers(new TwitchUsers)
    , pronouns(new pronouns::Pronouns)
    , spellChecker(new SpellChecker)
    , kickChatServer(new KickChatServer)
    , rumble(new RumbleApplicationController(QCoreApplication::instance(),
                                             &this->accounts->rumble))
#ifdef CHATTERINO_HAVE_PLUGINS
    , plugins(new PluginController(paths))
#endif
    , nmServer(new NativeMessagingServer())
    , updates(_updates)
{
}

Application::~Application()
{
    // we do this early to ensure getApp isn't used in any dtors
    INSTANCE = nullptr;
}

void Application::initialize(Settings &settings, const Modes &modes,
                             const Paths &paths)
{
    assert(!this->initialized);

    // Show changelog
    if (!this->args_.isFramelessEmbed &&
        getSettings()->currentVersion.getValue() != "" &&
        getSettings()->currentVersion.getValue() != CHATTERINO_VERSION)
    {
        auto *box = new QMessageBox(QMessageBox::Information, "Chatterino 7TV",
                                    "Show changelog?",
                                    QMessageBox::Yes | QMessageBox::No);
        box->setAttribute(Qt::WA_DeleteOnClose);
        if (box->exec() == QMessageBox::Yes)
        {
            QDesktopServices::openUrl(
                QUrl("https://www.chatterino.com/changelog"));
        }
    }

    if (!this->args_.isFramelessEmbed)
    {
        getSettings()->currentVersion.setValue(CHATTERINO_VERSION);
    }
    this->emotes->initialize();

    this->accounts->load();

    this->windows->initialize();

    this->ffzBadges->load();

    // Load global emotes
    this->bttvEmotes->loadEmotes();
    this->ffzEmotes->loadEmotes();
    this->seventvEmotes->loadGlobalEmotes();

    this->twitch->initialize();
    this->kickChatServer->initialize();

    // Load live status
    this->notifications->initialize();

    // XXX: Loading Twitch badges after Helix has been initialized, which only happens after
    // the AccountController initialize has been called
    this->twitchBadges->loadTwitchBadges();

#ifdef CHATTERINO_HAVE_PLUGINS
    this->plugins->initialize(settings);
#endif

    // Show crash message.
    // On Windows, the crash message was already shown.
#ifndef Q_OS_WIN
    if (!this->args_.isFramelessEmbed && this->args_.crashRecovery)
    {
        if (auto *selected =
                this->windows->getMainWindow().getNotebook().getSelectedPage())
        {
            if (auto *container = dynamic_cast<SplitContainer *>(selected))
            {
                for (auto &&split : container->getSplits())
                {
                    if (auto channel = split->getChannel(); !channel->isEmpty())
                    {
                        channel->addSystemMessage(
                            "Chatterino unexpectedly crashed and restarted. "
                            "You can disable automatic restarts in the "
                            "settings.");
                    }
                }
            }
        }
    }
#endif

    if (!this->args_.isFramelessEmbed)
    {
        this->initNm(modes, paths);
    }

    this->twitch->initEventAPIs(this->bttvLiveUpdates.get(),
                                this->seventvEventAPI.get());

    this->streamerMode->start();

    this->initialized = true;
}

int Application::run()
{
    assert(this->initialized);

    this->twitch->connect();

    if (!this->args_.isFramelessEmbed)
    {
        this->windows->getMainWindow().show();
    }
    installRumbleUsercardTestFixture();
    installRumbleReplyTestFixture();
    installRumbleLifecycleTestFixture();
    installAutomaticRoutingTestFixture();
    installCompactTabRevealTestFixture();

    getSettings()->enableBTTVChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllBTTVChannelEmotes();
        },
        false);
    getSettings()->enableFFZChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllFFZChannelEmotes();
        },
        false);
    getSettings()->enableSevenTVChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllSevenTVChannelEmotes();
        },
        false);

    return QApplication::exec();
}

Theme *Application::getThemes()
{
    assertInGuiThread();
    assert(this->themes);

    return this->themes.get();
}

Fonts *Application::getFonts()
{
    assertInGuiThread();
    assert(this->fonts);

    return this->fonts.get();
}

EmoteController *Application::getEmotes()
{
    assertInGuiThread();
    assert(this->emotes);

    return this->emotes.get();
}

AccountController *Application::getAccounts()
{
    assertInGuiThread();
    assert(this->accounts);

    return this->accounts.get();
}

HotkeyController *Application::getHotkeys()
{
    assertInGuiThread();
    assert(this->hotkeys);

    return this->hotkeys.get();
}

WindowManager *Application::getWindows()
{
    assertInGuiThread();
    assert(this->windows);

    return this->windows.get();
}

Toasts *Application::getToasts()
{
    assertInGuiThread();
    assert(this->toasts);

    return this->toasts.get();
}

CrashHandler *Application::getCrashHandler()
{
    assertInGuiThread();
    assert(this->crashHandler);

    return this->crashHandler.get();
}

CommandController *Application::getCommands()
{
    assertInGuiThread();
    assert(this->commands);

    return this->commands.get();
}

NotificationController *Application::getNotifications()
{
    assertInGuiThread();
    assert(this->notifications);

    return this->notifications.get();
}

HighlightController *Application::getHighlights()
{
    assertInGuiThread();
    assert(this->highlights);

    return this->highlights.get();
}

FfzBadges *Application::getFfzBadges()
{
    assertInGuiThread();
    assert(this->ffzBadges);

    return this->ffzBadges.get();
}

BttvBadges *Application::getBttvBadges()
{
    // BttvBadges handles its own locks, so we don't need to assert that this is called in the GUI thread
    assert(this->bttvBadges);

    return this->bttvBadges.get();
}

SeventvBadges *Application::getSeventvBadges()
{
    // SeventvBadges handles its own locks, so we don't need to assert that this is called in the GUI thread
    assert(this->seventvBadges);

    return this->seventvBadges.get();
}

IUserDataController *Application::getUserData()
{
    assertInGuiThread();

    return this->userData.get();
}

ISoundController *Application::getSound()
{
    assertInGuiThread();

    return this->sound.get();
}

ITwitchLiveController *Application::getTwitchLiveController()
{
    assertInGuiThread();
    assert(this->twitchLiveController);

    return this->twitchLiveController.get();
}

TwitchBadges *Application::getTwitchBadges()
{
    assertInGuiThread();
    assert(this->twitchBadges);

    return this->twitchBadges.get();
}

IChatterinoBadges *Application::getChatterinoBadges()
{
    assertInGuiThread();
    assert(this->chatterinoBadges);

    return this->chatterinoBadges.get();
}

ImageUploader *Application::getImageUploader()
{
    assertInGuiThread();
    assert(this->imageUploader);

    return this->imageUploader.get();
}

SeventvAPI *Application::getSeventvAPI()
{
    assertInGuiThread();
    assert(this->seventvAPI);

    return this->seventvAPI.get();
}

#ifdef CHATTERINO_HAVE_PLUGINS
PluginController *Application::getPlugins()
{
    assertInGuiThread();
    assert(this->plugins);

    return this->plugins.get();
}
#endif

Updates &Application::getUpdates()
{
    assertInGuiThread();

    return this->updates;
}

ITwitchIrcServer *Application::getTwitch()
{
    return this->twitch.get();
}

PubSub *Application::getTwitchPubSub()
{
    assertInGuiThread();

    return this->twitchPubSub.get();
}

ILogging *Application::getChatLogger()
{
    assertInGuiThread();
    assert(this->logging);

    return this->logging.get();
}

ILinkResolver *Application::getLinkResolver()
{
    assertInGuiThread();

    return this->linkResolver.get();
}

IStreamerMode *Application::getStreamerMode()
{
    return this->streamerMode.get();
}

ITwitchUsers *Application::getTwitchUsers()
{
    assertInGuiThread();
    assert(this->twitchUsers);

    return this->twitchUsers.get();
}

BttvEmotes *Application::getBttvEmotes()
{
    assertInGuiThread();
    assert(this->bttvEmotes);

    return this->bttvEmotes.get();
}

BttvLiveUpdates *Application::getBttvLiveUpdates()
{
    assertInGuiThread();
    // bttvLiveUpdates may be nullptr if it's not enabled

    return this->bttvLiveUpdates.get();
}

FfzEmotes *Application::getFfzEmotes()
{
    assertInGuiThread();
    assert(this->ffzEmotes);

    return this->ffzEmotes.get();
}

SeventvEmotes *Application::getSeventvEmotes()
{
    assertInGuiThread();
    assert(this->seventvEmotes);

    return this->seventvEmotes.get();
}

SeventvPersonalEmotes *Application::getSeventvPersonalEmotes()
{
    assert(this->seventvPersonalEmotes);

    return this->seventvPersonalEmotes.get();
}

SeventvPaints *Application::getSeventvPaints()
{
    assert(this->seventvPaints);

    return this->seventvPaints.get();
}

SeventvEventAPI *Application::getSeventvEventAPI()
{
    assertInGuiThread();
    // seventvEventAPI may be nullptr if it's not enabled

    return this->seventvEventAPI.get();
}

pronouns::Pronouns *Application::getPronouns()
{
    // pronouns::Pronouns handles its own locks, so we don't need to assert that this is called in the GUI thread
    assert(this->pronouns);

    return this->pronouns.get();
}

eventsub::IController *Application::getEventSub()
{
    assert(this->eventSub);

    return this->eventSub.get();
}

SpellChecker *Application::getSpellChecker()
{
    assertInGuiThread();
    assert(this->spellChecker);

    return this->spellChecker.get();
}

KickChatServer *Application::getKickChatServer()
{
    assertInGuiThread();
    assert(this->kickChatServer);

    return this->kickChatServer.get();
}

RumbleApplicationController *Application::getRumble()
{
    assertInGuiThread();
    assert(this->rumble);
    return this->rumble.get();
}

void Application::aboutToQuit()
{
    ABOUT_TO_QUIT.store(true);

    // Close public-resolution/picker gates before layout serialization or any
    // widget/provider teardown. The retained locator lives in each view and is
    // therefore still available to WindowManager::save().
    if (this->rumble)
    {
        this->rumble->beginShutdown();
    }

    this->eventSub->setQuitting();

    this->twitch->aboutToQuit();

    this->hotkeys->save();
    this->windows->save();

    this->windows->closeAll();
}

void Application::stop()
{
    if (this->rumble)
    {
        this->rumble->shutdown();
        this->rumble.reset();
    }
#ifdef CHATTERINO_HAVE_PLUGINS
    this->plugins.reset();
#endif
    this->pronouns.reset();
    this->twitchUsers.reset();
    this->streamerMode.reset();
    this->linkResolver.reset();
    this->seventvEventAPI.reset();
    this->seventvEmotes.reset();
    this->ffzEmotes.reset();
    this->bttvLiveUpdates.reset();
    this->bttvEmotes.reset();
    this->chatterinoBadges.reset();
    this->twitchBadges.reset();
    this->twitchPubSub.reset();
    this->twitchLiveController.reset();
    this->sound.reset();
    this->userData.reset();
    this->seventvBadges.reset();
    this->ffzBadges.reset();
    this->twitch.reset();
    this->highlights.reset();
    this->notifications.reset();
    this->commands.reset();
    this->crashHandler.reset();
    this->seventvAPI.reset();
    this->imageUploader.reset();
    this->toasts.reset();
    this->windows.reset();
    this->hotkeys.reset();
    this->eventSub.reset();
    this->accounts.reset();
    this->emotes.reset();
    this->logging.reset();
    this->fonts.reset();
    this->themes.reset();
    this->spellChecker.reset();

    STOPPED.store(true);
}

void Application::initNm(const Modes &modes, const Paths &paths)
{
    (void)modes;
    (void)paths;

#if defined QT_NO_DEBUG || defined CHATTERINO_DEBUG_NM
    registerNmHost(modes, paths);
    this->nmServer->start();
#endif
}

IApplication *getApp()
{
    assert(INSTANCE != nullptr);
    assert(STOPPED.load() == false);

    return INSTANCE;
}

IApplication *tryGetApp()
{
    return INSTANCE;
}

bool isAppAboutToQuit()
{
    return ABOUT_TO_QUIT.load();
}

}  // namespace chatterino
