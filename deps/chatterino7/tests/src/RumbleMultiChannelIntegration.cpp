// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/WindowDescriptors.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/strategies/Strategy.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/rumble/RumbleChannel.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/dialogs/SelectChannelDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS
#    include <sol/sol.hpp>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace chatterino;

namespace {

class IntegrationTwitchIrcServer final : public mock::MockTwitchIrcServer
{
public:
    ChannelPtr getOrAddChannel(const QString &name) override
    {
        return std::make_shared<Channel>(name, Channel::Type::Twitch);
    }

    ChannelPtr getChannelOrEmpty(const QString &name) override
    {
        return std::make_shared<Channel>(name, Channel::Type::Misc);
    }
};

class IntegrationApplication final : public mock::BaseApplication
{
public:
    IntegrationApplication()
        : windows(this->args_, this->paths_, this->settings, this->theme,
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
        return &this->windows;
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
        return &this->bttv;
    }

    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffz;
    }

    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventv;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    IntegrationTwitchIrcServer twitch;
    mock::EmptyLogging logging;
    HotkeyController hotkeys;
    WindowManager windows;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
    BttvEmotes bttv;
    FfzEmotes ffz;
    SeventvEmotes seventv;
};

class MutableChannel final : public Channel
{
public:
    MutableChannel(QString name, MultiChannel::Platform platform)
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
        this->context.accountID = this->context.platform +
                                  QStringLiteral("-account");
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
        ++this->contextReads;
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
        {
            this->pendingSend = std::move(callback);
        }
        else if (callback)
        {
            callback({SendOutcome::Confirmed, {}});
        }
    }

    MessageSendContext context;
    mutable size_t contextReads = 0;
    std::vector<QString> sent;
    bool deferSend = false;
    SendCallback pendingSend;
};

class InlineDispatcher final : public RumbleDispatcher
{
public:
    bool isOwnerThread() const noexcept override
    {
        return true;
    }

    bool dispatch(Task task) override
    {
        task();
        return true;
    }

    void dispose(Task cleanup) noexcept override
    {
        cleanup();
    }
};

class CopyEmoteStrategy final : public completion::Strategy<completion::EmoteItem>
{
public:
    void apply(const std::vector<completion::EmoteItem> &items,
               std::vector<completion::EmoteItem> &output,
               const QString &) const override
    {
        output = items;
    }
};

class TestableSplitInput final : public SplitInput
{
public:
    using SplitInput::handleSendMessage;
    using SplitInput::SplitInput;

    void setReply(const MessagePtr &reply, const ChannelPtr &channel)
    {
        this->replyTarget_ = reply;
        this->replyChannel_ = channel;
    }

    void setPickerReply(const MessagePtr &reply, const ChannelPtr &channel)
    {
        SplitInput::setReply(reply, channel);
    }

    void setOverride(QString platform)
    {
        this->setRoutingPlatformOverride(std::move(platform));
    }

    void clearReply()
    {
        this->replyTarget_.reset();
        this->replyChannel_.reset();
    }

    bool hasReply() const
    {
        return this->replyTarget_ != nullptr;
    }
};

MultiChannel::Spec twitchSpec(QString name)
{
    return {
        .platform = MultiChannel::Platform::Twitch,
        .name = std::move(name),
    };
}

MultiChannel::Spec kickSpec(QString name)
{
    return {
        .platform = MultiChannel::Platform::Kick,
        .name = std::move(name),
    };
}

MultiChannel::Spec rumbleSpec(QString locator)
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

std::shared_ptr<MultiChannel> makeMulti(
    std::span<const MultiChannel::Spec> specs,
    std::vector<ChannelPtr> channels)
{
    auto next = std::make_shared<size_t>(0);
    return std::make_shared<MultiChannel>(
        specs, MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [channels = std::move(channels), next](const MultiChannel::Spec &)
            mutable -> ChannelPtr {
            if (*next >= channels.size())
            {
                return Channel::getEmpty();
            }
            return channels[(*next)++];
        });
}

QPushButton *buttonWithText(QWidget &root, const QString &text)
{
    for (auto *button : root.findChildren<QPushButton *>())
    {
        if (button->text() == text)
        {
            return button;
        }
    }
    return nullptr;
}

QPushButton *dialogOk(SelectChannelDialog &dialog)
{
    auto *box = dialog.findChild<QDialogButtonBox *>();
    return box ? box->button(QDialogButtonBox::Ok) : nullptr;
}

MessagePtr message(QString id, qint64 received,
                   MessagePlatform platform = MessagePlatform::Rumble)
{
    auto result = std::make_shared<Message>();
    result->id = std::move(id);
    result->loginName = QStringLiteral("viewer");
    result->displayName = QStringLiteral("Viewer");
    result->channelName = QStringLiteral("source");
    result->platform = platform;
    result->serverReceivedTime =
        QDateTime::fromMSecsSinceEpoch(received, Qt::UTC);
    return result;
}

RumbleMessagePublication publication(const MessagePtr &message)
{
    auto result = RumbleMessagePublication::create(message, false);
    EXPECT_TRUE(result);
    return *result;
}

EmotePtr namedEmote(const QString &name, const QString &id)
{
    return std::shared_ptr<Emote>(new Emote{
        .name = EmoteName{name},
        .id = EmoteId{id},
    });
}

#ifdef CHATTERINO_HAVE_PLUGINS
std::vector<QJsonObject> emittedEvents(sol::state &lua)
{
    std::vector<QJsonObject> events;
    const sol::table sent = lua["sent"];
    for (size_t index = 1; index <= sent.size(); ++index)
    {
        const auto payload = sent[index].get<std::string>();
        const auto document =
            QJsonDocument::fromJson(QByteArray::fromStdString(payload));
        EXPECT_TRUE(document.isObject()) << payload;
        if (document.isObject())
        {
            events.push_back(document.object());
        }
    }
    return events;
}

const QJsonObject *lastEvent(const std::vector<QJsonObject> &events,
                             const QString &name)
{
    const auto found = std::find_if(
        events.rbegin(), events.rend(), [&name](const QJsonObject &event) {
            return event.value(QStringLiteral("event")).toString() == name;
        });
    return found == events.rend() ? nullptr : &*found;
}
#endif

}  // namespace

class RumbleMultiChannelIntegration : public ::testing::Test
{
protected:
    void TearDown() override
    {
        // The test runner drains deferred Qt work after RUN_ALL_TESTS(), when
        // this fixture's application is already gone. Drain it here instead
        // so queued widget cleanup still has a valid application context.
        for (size_t i = 0; i < 32; ++i)
        {
            QCoreApplication::processEvents(
                QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents(nullptr,
                                               QEvent::DeferredDelete);
        }
    }

    IntegrationApplication application;
};

TEST_F(RumbleMultiChannelIntegration,
       RumbleHeaderTracksTitlesWithoutRenamingTheChannel)
{
    this->application.settings.headerStreamTitle.setValue(true);
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("vtitle"));
    ASSERT_TRUE(created);
    auto channel = *created;
    auto operation = channel->beginOperation(RumbleOperationKind::Resolver);
    ASSERT_TRUE(operation);

    Split split(nullptr);
    split.setChannel(IndirectChannel{channel});
    auto *header = split.findChild<SplitHeader *>();
    ASSERT_NE(header, nullptr);

    int displayNameChanges = 0;
    int tabRefreshes = 0;
    auto displayConnection = channel->displayNameChanged.connect([&] {
        ++displayNameChanges;
    });
    auto tabConnection =
        split.actionRequested.connect([&](Split::Action action) {
            if (action == Split::Action::RefreshTab)
            {
                ++tabRefreshes;
            }
        });

    auto first = RumbleResolvedMetadata::create(
        QStringLiteral("Stable Channel"), std::nullopt, std::nullopt,
        std::nullopt, QStringLiteral("First Live Title"));
    ASSERT_TRUE(first);
    channel->publishMetadata(*operation, std::move(*first));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(channel->getDisplayName(), QStringLiteral("Stable Channel"));
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Channel (live) - First Live Title"));

    displayNameChanges = 0;
    tabRefreshes = 0;
    auto changed = RumbleResolvedMetadata::create(
        QStringLiteral("Stable Channel"), std::nullopt, std::nullopt,
        std::nullopt, QStringLiteral("Changed Live Title"));
    ASSERT_TRUE(changed);
    channel->publishMetadata(*operation, std::move(*changed));
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Channel (live) - Changed Live Title"));
    EXPECT_EQ(channel->getDisplayName(), QStringLiteral("Stable Channel"));
    EXPECT_EQ(displayNameChanges, 0);
    EXPECT_EQ(tabRefreshes, 0);

    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Backoff));
    EXPECT_EQ(channel->streamTitle(), QStringLiteral("Changed Live Title"));
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Channel (live) - Changed Live Title"));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Unresolved));
    EXPECT_TRUE(channel->streamTitle().isEmpty());
    ASSERT_TRUE(channel->metadata());
    EXPECT_TRUE(channel->metadata()->streamTitle().isEmpty());
    EXPECT_EQ(header->channelText(), QStringLiteral("Stable Channel (live)"));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Offline));
    EXPECT_EQ(header->channelText(), QStringLiteral("Stable Channel"));

    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Unresolved));
    auto reconnected = RumbleResolvedMetadata::create(
        QStringLiteral("Stable Channel"), std::nullopt, std::nullopt,
        std::nullopt, QStringLiteral("Next Stream Title"));
    ASSERT_TRUE(reconnected);
    channel->publishMetadata(*operation, std::move(*reconnected));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(channel->getDisplayName(), QStringLiteral("Stable Channel"));
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Channel (live) - Next Stream Title"));
}

TEST_F(RumbleMultiChannelIntegration,
       RumbleHeadersKeepIndependentTitlesAcrossTabs)
{
    this->application.settings.headerStreamTitle.setValue(true);
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto firstCreated = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                             QStringLiteral("vfirsttitle"));
    auto secondCreated = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                              QStringLiteral("vsecondtitle"));
    ASSERT_TRUE(firstCreated);
    ASSERT_TRUE(secondCreated);
    auto firstChannel = *firstCreated;
    auto secondChannel = *secondCreated;
    auto firstOperation =
        firstChannel->beginOperation(RumbleOperationKind::Resolver);
    auto secondOperation =
        secondChannel->beginOperation(RumbleOperationKind::Resolver);
    ASSERT_TRUE(firstOperation);
    ASSERT_TRUE(secondOperation);

    auto first = RumbleResolvedMetadata::create(
        QStringLiteral("Stable One"), std::nullopt, std::nullopt, std::nullopt,
        QStringLiteral("First Title"));
    auto second = RumbleResolvedMetadata::create(
        QStringLiteral("Stable Two"), std::nullopt, std::nullopt, std::nullopt,
        QStringLiteral("Second Title"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    firstChannel->publishMetadata(*firstOperation, std::move(*first));
    secondChannel->publishMetadata(*secondOperation, std::move(*second));
    ASSERT_TRUE(firstChannel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(firstChannel->transitionTo(RumbleChannelState::Connected));
    ASSERT_TRUE(secondChannel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(secondChannel->transitionTo(RumbleChannelState::Connected));

    Split firstSplit(nullptr);
    Split secondSplit(nullptr);
    firstSplit.setChannel(IndirectChannel{firstChannel});
    secondSplit.setChannel(IndirectChannel{secondChannel});
    auto *firstHeader = firstSplit.findChild<SplitHeader *>();
    auto *secondHeader = secondSplit.findChild<SplitHeader *>();
    ASSERT_NE(firstHeader, nullptr);
    ASSERT_NE(secondHeader, nullptr);
    EXPECT_EQ(firstHeader->channelText(),
              QStringLiteral("Stable One (live) - First Title"));
    EXPECT_EQ(secondHeader->channelText(),
              QStringLiteral("Stable Two (live) - Second Title"));
}

TEST_F(RumbleMultiChannelIntegration,
       MixedTabShowsSelectedRumbleTitleAndKeepsAggregateName)
{
    this->application.settings.headerStreamTitle.setValue(true);
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("vmixedtitle"));
    ASSERT_TRUE(created);
    auto rumble = *created;
    auto operation = rumble->beginOperation(RumbleOperationKind::Resolver);
    ASSERT_TRUE(operation);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Stable Rumble"), std::nullopt, std::nullopt,
        std::nullopt, QStringLiteral("Selected Live Title"));
    ASSERT_TRUE(metadata);
    rumble->publishMetadata(*operation, std::move(*metadata));
    ASSERT_TRUE(rumble->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(rumble->transitionTo(RumbleChannelState::Connected));

    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vmixedtitle")),
        twitchSpec(QStringLiteral("alpha")),
    };
    auto multi = makeMulti(specs, {rumble, twitch});
    const auto stableAggregateName = multi->getDisplayName();
    EXPECT_TRUE(stableAggregateName.contains(QStringLiteral("Stable Rumble")));
    EXPECT_FALSE(
        stableAggregateName.contains(QStringLiteral("Selected Live Title")));

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    auto *header = split.findChild<SplitHeader *>();
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Rumble (live) - Selected Live Title"));

    multi->setActiveChannelIndex(1);
    EXPECT_EQ(header->channelText(), QStringLiteral("alpha"));
    EXPECT_EQ(multi->getDisplayName(), stableAggregateName);
    multi->setActiveChannelIndex(0);
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Rumble (live) - Selected Live Title"));

    auto updated = RumbleResolvedMetadata::create(
        QStringLiteral("Stable Rumble"), std::nullopt, std::nullopt,
        std::nullopt, QStringLiteral("Updated Selected Title"));
    ASSERT_TRUE(updated);
    rumble->publishMetadata(*operation, std::move(*updated));
    EXPECT_EQ(header->channelText(),
              QStringLiteral("Stable Rumble (live) - Updated Selected Title"));
    EXPECT_EQ(multi->getDisplayName(), stableAggregateName);
}

TEST_F(RumbleMultiChannelIntegration, WindowManagerOwnsQueuedSaveTimer)
{
    const auto timers = application.windows.findChildren<QTimer *>(
        QString{}, Qt::FindDirectChildrenOnly);
    ASSERT_EQ(timers.size(), 1);
    auto *timer = timers.front();
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->parent(), &application.windows);
    EXPECT_TRUE(timer->isSingleShot());

    application.windows.queueSave();
    EXPECT_TRUE(timer->isActive());
}

TEST_F(RumbleMultiChannelIntegration,
       LayoutEditPreservesOrderLocatorsAndSelectedRow)
{
    const std::array specs{
        twitchSpec(QStringLiteral("alpha")),
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vfirst")),
        rumbleSpec(QStringLiteral("https://rumble.com/vsecond-title.html")),
    };
    auto original = makeMulti(
        specs,
        {
            std::make_shared<MutableChannel>(
                QStringLiteral("alpha"), MultiChannel::Platform::Twitch),
            std::make_shared<MutableChannel>(
                QStringLiteral("rumble-runtime-first"),
                MultiChannel::Platform::Rumble),
            std::make_shared<MutableChannel>(
                QStringLiteral("rumble-runtime-second"),
                MultiChannel::Platform::Rumble),
        });
    original->setActiveChannelIndex(1);

    SelectChannelDialog dialog;
    dialog.setSelectedChannel(IndirectChannel{original});
    auto *view = dialog.findChild<QListWidget *>();
    ASSERT_NE(view, nullptr);
    ASSERT_EQ(view->count(), 3);
    EXPECT_TRUE(view->item(1)->font().bold());

    auto *selected = view->takeItem(1);
    view->insertItem(0, selected);
    EXPECT_TRUE(view->item(0)->font().bold());
    ASSERT_NE(dialogOk(dialog), nullptr);
    dialogOk(dialog)->click();

    auto edited = std::dynamic_pointer_cast<MultiChannel>(
        dialog.getSelectedChannel().get());
    ASSERT_NE(edited, nullptr);
    ASSERT_EQ(edited->channels().size(), 3U);
    EXPECT_EQ(edited->channels()[0].spec().layoutIdentity->locator,
              specs[1].layoutIdentity->locator);
    EXPECT_EQ(edited->channels()[1].spec().name, QStringLiteral("alpha"));
    EXPECT_EQ(edited->channels()[2].spec().layoutIdentity->locator,
              specs[2].layoutIdentity->locator);
    EXPECT_EQ(edited->activeChannelIndex(), 0U);

    QJsonObject encoded;
    WindowManager::encodeChannel(IndirectChannel{edited}, encoded);
    ASSERT_EQ(encoded.value(QStringLiteral("children")).toArray().size(), 3);
    EXPECT_EQ(encoded.value(QStringLiteral("activeIndex")).toInt(), 0);
    SplitDescriptor restored;
    SplitDescriptor::loadFromJSON(restored, QJsonObject{}, encoded);
    ASSERT_EQ(restored.children.size(), 3U);
    EXPECT_EQ(restored.children[0].layoutIdentity->locator,
              specs[1].layoutIdentity->locator);
    EXPECT_EQ(restored.children[2].layoutIdentity->locator,
              specs[2].layoutIdentity->locator);

    // Exercise the persisted mixed-provider descriptor without asking the
    // production Kick registry to resolve a network-backed channel. The
    // injected channels represent the reordered dialog result at this seam.
    const std::array mixedSpecs{
        twitchSpec(QStringLiteral("alpha")),
        kickSpec(QStringLiteral("kick-row")),
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vmixed")),
    };
    auto reorderedMixed = makeMulti(
        mixedSpecs,
        {
            std::make_shared<MutableChannel>(
                QStringLiteral("alpha"), MultiChannel::Platform::Twitch),
            std::make_shared<MutableChannel>(
                QStringLiteral("kick-row"), MultiChannel::Platform::Kick),
            std::make_shared<MutableChannel>(
                QStringLiteral("rumble-secret-runtime-id"),
                MultiChannel::Platform::Rumble),
        });
    QJsonObject mixedEncoded;
    WindowManager::encodeChannel(IndirectChannel{reorderedMixed}, mixedEncoded);
    const auto mixedChildren =
        mixedEncoded.value(QStringLiteral("children")).toArray();
    ASSERT_EQ(mixedChildren.size(), 3);
    EXPECT_EQ(mixedChildren[0]
                  .toObject()
                  .value(QStringLiteral("platform"))
                  .toString(),
              QStringLiteral("Twitch"));
    EXPECT_EQ(mixedChildren[0]
                  .toObject()
                  .value(QStringLiteral("channel"))
                  .toString(),
              QStringLiteral("alpha"));
    EXPECT_EQ(mixedChildren[1]
                  .toObject()
                  .value(QStringLiteral("platform"))
                  .toString(),
              QStringLiteral("Kick"));
    EXPECT_EQ(mixedChildren[1]
                  .toObject()
                  .value(QStringLiteral("channel"))
                  .toString(),
              QStringLiteral("kick-row"));
    EXPECT_EQ(mixedChildren[2]
                  .toObject()
                  .value(QStringLiteral("platform"))
                  .toString(),
              QStringLiteral("Rumble"));
    EXPECT_EQ(mixedChildren[2]
                  .toObject()
                  .value(QStringLiteral("locator"))
                  .toString(),
              QStringLiteral("https://rumble.com/embed/vmixed"));

    SplitDescriptor mixedRestored;
    SplitDescriptor::loadFromJSON(mixedRestored, QJsonObject{}, mixedEncoded);
    ASSERT_EQ(mixedRestored.children.size(), 3U);
    EXPECT_EQ(mixedRestored.children[0].platform, QStringLiteral("Twitch"));
    EXPECT_EQ(mixedRestored.children[0].channelName,
              QStringLiteral("alpha"));
    EXPECT_EQ(mixedRestored.children[1].platform, QStringLiteral("Kick"));
    EXPECT_EQ(mixedRestored.children[1].channelName,
              QStringLiteral("kick-row"));
    EXPECT_EQ(mixedRestored.children[2].platform, QStringLiteral("Rumble"));
    ASSERT_TRUE(mixedRestored.children[2].layoutIdentity);
    EXPECT_EQ(mixedRestored.children[2].layoutIdentity->locator,
              QStringLiteral("https://rumble.com/embed/vmixed"));
}

TEST_F(RumbleMultiChannelIntegration,
       RemovedOrInvalidSelectionUsesDeterministicFallback)
{
    const std::array specs{
        twitchSpec(QStringLiteral("alpha")),
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vselected")),
        twitchSpec(QStringLiteral("omega")),
    };
    auto original = makeMulti(
        specs,
        {
            std::make_shared<MutableChannel>(
                QStringLiteral("alpha"), MultiChannel::Platform::Twitch),
            std::make_shared<MutableChannel>(
                QStringLiteral("runtime"), MultiChannel::Platform::Rumble),
            std::make_shared<MutableChannel>(
                QStringLiteral("omega"), MultiChannel::Platform::Twitch),
        });
    original->setActiveChannelIndex(1);

    SelectChannelDialog dialog;
    dialog.setSelectedChannel(IndirectChannel{original});
    auto *view = dialog.findChild<QListWidget *>();
    auto *remove = buttonWithText(dialog, QStringLiteral("Remove"));
    ASSERT_NE(view, nullptr);
    ASSERT_NE(remove, nullptr);
    view->setCurrentRow(1);
    remove->click();
    ASSERT_EQ(view->count(), 2);
    EXPECT_TRUE(view->item(1)->font().bold());
    dialogOk(dialog)->click();
    auto edited = std::dynamic_pointer_cast<MultiChannel>(
        dialog.getSelectedChannel().get());
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->activeChannelIndex(), 1U);
    EXPECT_EQ(edited->channels()[1].spec().name, QStringLiteral("omega"));

    auto finalSelected = makeMulti(
        specs,
        {
            std::make_shared<MutableChannel>(
                QStringLiteral("alpha"), MultiChannel::Platform::Twitch),
            std::make_shared<MutableChannel>(
                QStringLiteral("runtime"), MultiChannel::Platform::Rumble),
            std::make_shared<MutableChannel>(
                QStringLiteral("omega"), MultiChannel::Platform::Twitch),
        });
    finalSelected->setActiveChannelIndex(2);
    SelectChannelDialog finalDialog;
    finalDialog.setSelectedChannel(IndirectChannel{finalSelected});
    auto *finalView = finalDialog.findChild<QListWidget *>();
    auto *finalRemove =
        buttonWithText(finalDialog, QStringLiteral("Remove"));
    ASSERT_NE(finalView, nullptr);
    ASSERT_NE(finalRemove, nullptr);
    finalView->setCurrentRow(2);
    finalRemove->click();
    ASSERT_EQ(finalView->count(), 2);
    EXPECT_TRUE(finalView->item(1)->font().bold());
    dialogOk(finalDialog)->click();
    auto finalEdited = std::dynamic_pointer_cast<MultiChannel>(
        finalDialog.getSelectedChannel().get());
    ASSERT_NE(finalEdited, nullptr);
    EXPECT_EQ(finalEdited->activeChannelIndex(), 1U);
    EXPECT_EQ(finalEdited->channels()[1].spec().layoutIdentity->locator,
              specs[1].layoutIdentity->locator);

    edited->setActiveChannelIndex(999);
    EXPECT_EQ(edited->activeChannelIndex(), 1U);
    ASSERT_NE(edited->activeChannel(), nullptr);

    SplitDescriptor invalidSaved;
    SplitDescriptor::loadFromJSON(
        invalidSaved, QJsonObject{},
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("multi")},
            {QStringLiteral("activeIndex"), 999},
            {QStringLiteral("children"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("platform"),
                              QStringLiteral("Twitch")},
                             {QStringLiteral("channel"),
                              QStringLiteral("alpha")}},
                 QJsonObject{{QStringLiteral("platform"),
                              QStringLiteral("Rumble")},
                             {QStringLiteral("locator"),
                              QStringLiteral(
                                  "https://rumble.com/embed/vlast")}},
             }},
        });
    auto restored = std::dynamic_pointer_cast<MultiChannel>(
        invalidSaved.decodeChannel().get());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->activeChannelIndex(), 1U);
}

TEST_F(RumbleMultiChannelIntegration, EmptyAndMalformedLayoutsRemainSafeAndTyped)
{
    const std::span<const MultiChannel::Spec> noSpecs;
    auto empty = makeMulti(noSpecs, {});
    EXPECT_TRUE(empty->isEmpty());
    EXPECT_EQ(empty->activeChannel(), nullptr);
    EXPECT_EQ(empty->activeChannelIndex(), 0U);
    empty->setActiveChannelIndex(9000);
    EXPECT_EQ(empty->activeChannelIndex(), 0U);
    EXPECT_FALSE(empty->isMod());
    EXPECT_FALSE(empty->isBroadcaster());
    EXPECT_FALSE(empty->hasModRights());
    EXPECT_FALSE(empty->shouldIgnoreHighlights());
    EXPECT_TRUE(empty->getCurrentStreamID().isEmpty());

    Split split(nullptr);
    split.setChannel(IndirectChannel{empty});
    EXPECT_EQ(split.getSelectedChannel(), empty);
    EXPECT_FALSE(split.getSelectedLocator());

    SplitDescriptor malformed;
    SplitDescriptor::loadFromJSON(
        malformed, QJsonObject{},
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("multi")},
            {QStringLiteral("activeIndex"), 42},
            {QStringLiteral("children"),
             QJsonArray{
                 QJsonObject{
                     {QStringLiteral("platform"), QStringLiteral("Rumble")},
                     {QStringLiteral("channel"),
                      QStringLiteral("rumble-secret-runtime-id")},
                     {QStringLiteral("layoutIdentity"),
                      QJsonObject{
                          {QStringLiteral("platform"),
                           QStringLiteral("rumble")},
                          {QStringLiteral("locator"),
                           QStringLiteral(
                               "https://example.com/?secret=drop")},
                      }},
                 },
                 QJsonObject{
                     {QStringLiteral("platform"),
                      QStringLiteral("UnknownFutureProvider")},
                     {QStringLiteral("channel"), QStringLiteral("ignored")},
                 },
             }},
        });
    auto typed =
        std::dynamic_pointer_cast<MultiChannel>(malformed.decodeChannel().get());
    ASSERT_NE(typed, nullptr);
    ASSERT_EQ(typed->channels().size(), 1U);
    EXPECT_EQ(typed->channels()[0].platform,
              MultiChannel::Platform::Rumble);
    ASSERT_TRUE(typed->channels()[0].spec().layoutIdentity);
    EXPECT_TRUE(typed->channels()[0].spec().layoutIdentity->locator.isEmpty());
    EXPECT_EQ(typed->activeChannelIndex(), 0U);
    EXPECT_NE(typed->activeChannel(), nullptr);
}

TEST_F(RumbleMultiChannelIntegration,
       SharedRuntimeAliasesBindMessagesAndRetryOnce)
{
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("vsame"));
    ASSERT_TRUE(created);
    auto runtime = *created;
    int retries = 0;
    ASSERT_TRUE(runtime->setReconnectDelegate([&retries] {
        ++retries;
    }));

    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vsame")),
        rumbleSpec(QStringLiteral("https://rumble.com/vsame-title.html")),
    };
    auto multi = makeMulti(specs, {runtime, runtime});
    ASSERT_EQ(multi->channels().size(), 2U);
    EXPECT_TRUE(multi->channels()[0].primaryRuntimeChannel);
    EXPECT_FALSE(multi->channels()[1].primaryRuntimeChannel);
    EXPECT_FALSE(multi->channels()[0].rumbleRoutingSessionAvailable);
    EXPECT_FALSE(multi->channels()[1].rumbleRoutingSessionAvailable);

    runtime->addMessage(message(QStringLiteral("once"), 10),
                        MessageContext::Original);
    EXPECT_EQ(multi->countMessages(), 1U);
    multi->reconnect();
    EXPECT_EQ(retries, 1);

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    int liveRefreshes = 0;
    auto liveConnection = split.getChannelView().liveStatusChanged.connect([&] {
        ++liveRefreshes;
    });
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connecting));
    EXPECT_FALSE(multi->channels()[0].rumbleRoutingSessionAvailable);
    EXPECT_FALSE(multi->channels()[1].rumbleRoutingSessionAvailable);
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connected));
    EXPECT_TRUE(multi->channels()[0].rumbleRoutingSessionAvailable);
    EXPECT_TRUE(multi->channels()[1].rumbleRoutingSessionAvailable);
    EXPECT_EQ(liveRefreshes, 1);
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Offline));
    EXPECT_FALSE(multi->channels()[0].rumbleRoutingSessionAvailable);
    EXPECT_FALSE(multi->channels()[1].rumbleRoutingSessionAvailable);
}

TEST_F(RumbleMultiChannelIntegration,
       MixedBootstrapRealtimeIsOrderedAndDeduplicated)
{
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("vordered"));
    ASSERT_TRUE(created);
    auto runtime = *created;
    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    twitch->addMessage(
        message(QStringLiteral("t20"), 20, MessagePlatform::AnyOrTwitch),
        MessageContext::Original);

    const std::array specs{
        twitchSpec(QStringLiteral("alpha")),
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vordered")),
    };
    auto multi = makeMulti(specs, {twitch, runtime});
    auto token = runtime->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(token);

    const auto r30 = message(QStringLiteral("r30"), 30);
    runtime->publishRealtime(*token, publication(r30));
    runtime->publishBootstrap(
        *token,
        {publication(message(QStringLiteral("r10"), 10)), publication(r30)});
    runtime->publishRealtime(*token, publication(r30));

    const auto snapshot = multi->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 3U);
    EXPECT_EQ(snapshot[0]->id, QStringLiteral("r10"));
    EXPECT_EQ(snapshot[1]->id, QStringLiteral("t20"));
    EXPECT_EQ(snapshot[2]->id, QStringLiteral("r30"));
    EXPECT_EQ(snapshot[0]->platform, MessagePlatform::Rumble);
    EXPECT_EQ(snapshot[2]->platform, MessagePlatform::Rumble);
}

TEST_F(RumbleMultiChannelIntegration,
       SelectionAndLiveChangesRefreshAggregateState)
{
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                        QStringLiteral("vlive"));
    ASSERT_TRUE(created);
    auto runtime = *created;
    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vlive")),
        twitchSpec(QStringLiteral("alpha")),
    };
    auto multi = makeMulti(specs, {runtime, twitch});
    EXPECT_FALSE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    int selectionChanges = 0;
    int aggregateChanges = 0;
    int tabRefreshes = 0;
    int viewLiveRefreshes = 0;
    auto selectionConnection = multi->activeChannelChanged.connect([&] {
        ++selectionChanges;
    });
    auto aggregateConnection = multi->childStateChanged.connect([&] {
        ++aggregateChanges;
    });
    auto splitConnection = split.actionRequested.connect(
        [&](Split::Action action) {
            if (action == Split::Action::RefreshTab)
            {
                ++tabRefreshes;
            }
        });
    auto viewConnection = split.getChannelView().liveStatusChanged.connect([&] {
        ++viewLiveRefreshes;
    });

    multi->setActiveChannelIndex(1);
    EXPECT_EQ(selectionChanges, 1);
    EXPECT_EQ(viewLiveRefreshes, 1);
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connecting));
    EXPECT_FALSE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connected));
    EXPECT_TRUE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    EXPECT_EQ(aggregateChanges, 2);
    EXPECT_EQ(tabRefreshes, 2);
    EXPECT_EQ(viewLiveRefreshes, 2);
    EXPECT_TRUE(multi->isLive());
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Backoff));
    EXPECT_TRUE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    EXPECT_TRUE(multi->isLive());
    EXPECT_EQ(viewLiveRefreshes, 2);
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Unresolved));
    EXPECT_TRUE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    EXPECT_TRUE(multi->isLive());
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connecting));
    EXPECT_TRUE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Offline));
    EXPECT_FALSE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    EXPECT_FALSE(multi->isLive());
    EXPECT_EQ(viewLiveRefreshes, 3);
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Unresolved));
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connecting));
    EXPECT_FALSE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    ASSERT_TRUE(runtime->transitionTo(RumbleChannelState::Connected));
    EXPECT_TRUE(
        multi->isRoutingPlatformAvailable(MultiChannel::Platform::Rumble));
    EXPECT_TRUE(multi->isLive());
    EXPECT_EQ(viewLiveRefreshes, 4);
}

TEST_F(RumbleMultiChannelIntegration,
       TabEmitUsesCanonicalLocatorAndNeverRuntimeIdentity)
{
    auto runtime = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-secret-runtime-id"),
        MultiChannel::Platform::Rumble);

    const std::array aliasSpecs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/valias")),
        rumbleSpec(QStringLiteral("https://rumble.com/valias-live.html")),
    };
    auto aliased = makeMulti(aliasSpecs, {runtime, runtime});
    aliased->setActiveChannelIndex(1);
    Split nestedSplit(nullptr);
    nestedSplit.setChannel(IndirectChannel{aliased});
    ASSERT_TRUE(nestedSplit.getSelectedLocator());
    const auto selectedAliasLocator = *nestedSplit.getSelectedLocator();
    EXPECT_EQ(selectedAliasLocator, aliasSpecs[1].layoutIdentity->locator);

    Split productionSplit(nullptr);
    productionSplit.setChannel(IndirectChannel{
        runtime, Channel::Type::Rumble,
        ChannelLayoutIdentity{
            .platform = QStringLiteral("rumble"),
            .locator = QStringLiteral(
                "https://rumble.com/embed/vcanonical?secret=drop"),
        }});
    ASSERT_TRUE(productionSplit.getSelectedLocator());
    EXPECT_EQ(*productionSplit.getSelectedLocator(),
              QStringLiteral("https://rumble.com/embed/vcanonical"));

#ifndef CHATTERINO_HAVE_PLUGINS
    GTEST_SKIP() << "embedded Lua is unavailable in this build";
#else
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                       sol::lib::math);
    lua["canonical_locator"] = selectedAliasLocator.toStdString();
    lua.script(R"lua(
        sent = {}
        handlers = nil
        commands = {}
        rumble_channel = {
            is_valid = function(_) return true end,
            get_display_name = function(_) return "Resolved Rumble Name" end,
            get_type = function(_) return 12 end,
            get_name = function(_) error("Rumble runtime get_name leaked") end,
        }
        setmetatable(rumble_channel, {
            __tostring = function(_) error("Rumble runtime tostring leaked") end,
        })
        split = {
            selected_platform = "rumble",
            selected_locator = canonical_locator,
            selected_is_live = true,
            selected_channel = rumble_channel,
            channel = rumble_channel,
            auto_select_context_by_recent_messages = function(_, _) return false end,
        }
        page = {
            selected_split = split,
            splits = function(_) return {split} end,
        }
        notebook = {
            selected_page = page,
            page_count = 1,
            page_at = function(_, index) if index == 0 then return page end end,
        }
        window = {notebook = notebook}
        c2 = {
            ChannelType = {Rumble = 12},
            register_command = function(name, callback)
                commands[name] = callback
                return true
            end,
            later = function(_, _) error("unexpected reconnect timer") end,
            windows = {
                last_selected_window = window,
                main_window = window,
                all = function(_) return {window} end,
            },
            WebSocket = {
                new = function(_, callbacks)
                    handlers = callbacks
                    return {
                        send_text = function(_, payload)
                            table.insert(sent, payload)
                        end,
                    }
                end,
            },
        }
    )lua");

    const QDir repository(QStringLiteral(CHATTERINO_PLUGIN_REPOSITORY_ROOT));
    const auto path = repository.filePath(QStringLiteral("tab-emit/init.lua"));
    auto loaded = lua.load_file(path.toStdString());
    ASSERT_TRUE(loaded.valid()) << path.toStdString();
    sol::protected_function script = loaded;
    const auto executed = script();
    ASSERT_TRUE(executed.valid());
    lua.script("handlers.on_open()");
    lua.script("commands['/tabemit-check']({channel = rumble_channel})");

    auto events = emittedEvents(lua);
    const auto *initial = lastEvent(events, QStringLiteral("initial_state"));
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->value(QStringLiteral("platform")).toString(),
              QStringLiteral("rumble"));
    EXPECT_EQ(initial->value(QStringLiteral("channel")).toString(),
              selectedAliasLocator);
    EXPECT_EQ(initial->value(QStringLiteral("stream_url")).toString(),
              selectedAliasLocator);
    EXPECT_EQ(initial->value(QStringLiteral("display_name")).toString(),
              QStringLiteral("Resolved Rumble Name"));
    EXPECT_TRUE(initial->value(QStringLiteral("channel_object")).isNull());
    EXPECT_TRUE(initial->value(QStringLiteral("state_ok")).toBool());
    EXPECT_TRUE(initial->value(QStringLiteral("is_live")).toBool());
    for (const auto &event : events)
    {
        EXPECT_FALSE(QJsonDocument(event).toJson().contains(
            QByteArrayLiteral("rumble-secret-runtime-id")));
    }

    lua.script("split.selected_is_live = false; handlers.on_text('check')");
    events = emittedEvents(lua);
    const auto *offline = lastEvent(events, QStringLiteral("tab_changed"));
    ASSERT_NE(offline, nullptr);
    EXPECT_FALSE(offline->value(QStringLiteral("is_live")).toBool());
    EXPECT_TRUE(offline->value(QStringLiteral("state_ok")).toBool());
    const auto offlineEventCount = events.size();
    lua.script("handlers.on_text('tick')");
    EXPECT_EQ(emittedEvents(lua).size(), offlineEventCount);

    lua.script("split.selected_locator = nil; handlers.on_text('check')");
    events = emittedEvents(lua);
    const auto *invalid = lastEvent(events, QStringLiteral("tab_changed"));
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(invalid->value(QStringLiteral("platform")).toString(),
              QStringLiteral("rumble"));
    EXPECT_TRUE(invalid->value(QStringLiteral("channel")).isNull());
    EXPECT_TRUE(invalid->value(QStringLiteral("stream_url")).isNull());
    EXPECT_FALSE(invalid->value(QStringLiteral("state_ok")).toBool());

    lua.script(R"lua(
        twitch_channel = {
            is_valid = function(_) return true end,
            get_name = function(_) return "stable_twitch" end,
            get_display_name = function(_) return "Stable Twitch" end,
            get_type = function(_) return "twitch" end,
        }
        setmetatable(twitch_channel, {__tostring = function(_) return "twitch-object" end})
        split.selected_platform = "twitch"
        split.selected_is_live = true
        split.selected_channel = twitch_channel
        split.channel = twitch_channel
        handlers.on_text("check")
    )lua");
    events = emittedEvents(lua);
    const auto *twitch = lastEvent(events, QStringLiteral("tab_changed"));
    ASSERT_NE(twitch, nullptr);
    EXPECT_EQ(twitch->value(QStringLiteral("channel")).toString(),
              QStringLiteral("stable_twitch"));
    EXPECT_EQ(twitch->value(QStringLiteral("stream_url")).toString(),
              QStringLiteral("https://www.twitch.tv/stable_twitch"));
    EXPECT_EQ(twitch->value(QStringLiteral("channel_object")).toString(),
              QStringLiteral("twitch-object"));
    EXPECT_TRUE(twitch->value(QStringLiteral("is_live")).isNull());

    lua.script(R"lua(
        kick_channel = {
            is_valid = function(_) return true end,
            get_name = function(_) return "stable_kick" end,
            get_display_name = function(_) return "Stable Kick" end,
            get_type = function(_) return "kick" end,
        }
        setmetatable(kick_channel, {__tostring = function(_) return "kick-object" end})
        split.selected_platform = "kick"
        split.selected_is_live = true
        split.selected_channel = kick_channel
        split.channel = kick_channel
        handlers.on_text("check")
    )lua");
    events = emittedEvents(lua);
    const auto *kick = lastEvent(events, QStringLiteral("tab_changed"));
    ASSERT_NE(kick, nullptr);
    EXPECT_EQ(kick->value(QStringLiteral("channel")).toString(),
              QStringLiteral("stable_kick"));
    EXPECT_EQ(kick->value(QStringLiteral("stream_url")).toString(),
              QStringLiteral("https://kick.com/stable_kick"));
    EXPECT_EQ(kick->value(QStringLiteral("channel_object")).toString(),
              QStringLiteral("kick-object"));
    EXPECT_TRUE(kick->value(QStringLiteral("is_live")).isNull());
#endif
}

TEST_F(RumbleMultiChannelIntegration,
       CompletionAndSubmitReadCurrentCapabilities)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vfresh")),
        twitchSpec(QStringLiteral("alpha")),
    };
    auto multi = makeMulti(specs, {rumble, twitch});

    auto map = std::make_shared<EmoteMap>();
    map->insert({EmoteName{QStringLiteral("Fresh")},
                 namedEmote(QStringLiteral("Fresh"),
                            QStringLiteral("fresh-id"))});
    this->application.bttv.setEmotes(std::move(map));

    completion::EmoteSource source(
        multi.get(), std::make_unique<CopyEmoteStrategy>());
    source.setInputContext(QString{});
    source.update(QStringLiteral("Fresh"));
    EXPECT_TRUE(source.output().empty());
    const auto beforeCompletionReads = twitch->contextReads;

    twitch->context.writable = true;
    twitch->context.authenticated = true;
    source.update(QStringLiteral("Fresh"));
    EXPECT_GT(twitch->contextReads, beforeCompletionReads);
    ASSERT_FALSE(source.output().empty());

    const auto beforeSubmitReads = twitch->contextReads;
    auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("fresh state")), 0);
    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 1U);
    EXPECT_GT(twitch->contextReads, beforeSubmitReads);
    EXPECT_EQ(twitch->sent,
              std::vector<QString>{QStringLiteral("fresh state")});
}

TEST_F(RumbleMultiChannelIntegration,
       AcceptedDraftDispatchesToExactlyOneChild)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    auto beta = std::make_shared<MutableChannel>(
        QStringLiteral("beta"), MultiChannel::Platform::Twitch);
    auto gamma = std::make_shared<MutableChannel>(
        QStringLiteral("gamma"), MultiChannel::Platform::Twitch);
    beta->context.writable = beta->context.authenticated = true;
    gamma->context.writable = gamma->context.authenticated = true;
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vdispatch")),
        twitchSpec(QStringLiteral("beta")),
        twitchSpec(QStringLiteral("gamma")),
    };
    auto multi = makeMulti(specs, {rumble, beta, gamma});

    beta->addMessage(message(QStringLiteral("beta-live"), 10,
                             MessagePlatform::AnyOrTwitch),
                     MessageContext::Original);
    gamma->addMessage(message(QStringLiteral("gamma-live"), 20,
                              MessagePlatform::AnyOrTwitch),
                      MessageContext::Original);
    auto result = multi->sendMessageDraft(
        MessageDraft::fromPlainText(QStringLiteral("exactly once")), 0);

    ASSERT_TRUE(result.sent());
    EXPECT_EQ(result.destinationIndex, 2U);
    EXPECT_TRUE(rumble->sent.empty());
    EXPECT_TRUE(beta->sent.empty());
    EXPECT_EQ(gamma->sent,
              std::vector<QString>{QStringLiteral("exactly once")});
}

TEST_F(RumbleMultiChannelIntegration,
       LoggedOutRumbleReplyBecomesOrdinaryDraftAndRetries)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    twitch->context.writable = twitch->context.authenticated = true;
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vunsupported")),
        twitchSpec(QStringLiteral("alpha")),
    };
    auto multi = makeMulti(specs, {rumble, twitch});

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());
    auto reply = message(QStringLiteral("reply-id"), 1);
    input.setInputText(QStringLiteral("reply body"));
    input.setReply(reply, rumble);
    const auto replyBefore = input.getInputText();
    const auto replyFailure = input.handleSendMessage({});
    EXPECT_TRUE(replyFailure.contains(QStringLiteral("Message not sent")));
    EXPECT_FALSE(replyFailure.contains(QStringLiteral("Reply not sent")));
    EXPECT_FALSE(replyFailure.contains(QStringLiteral("not supported")));
    EXPECT_EQ(input.getInputText(), replyBefore);
    EXPECT_FALSE(input.hasReply());
    EXPECT_TRUE(rumble->sent.empty());
    EXPECT_TRUE(twitch->sent.empty());

    rumble->context.writable = rumble->context.authenticated = true;
    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
    EXPECT_THAT(rumble->sent, ::testing::ElementsAre(replyBefore));
    EXPECT_TRUE(twitch->sent.empty());
    EXPECT_TRUE(input.getInputText().isEmpty());
}

TEST_F(RumbleMultiChannelIntegration,
       UnsupportedProviderCommandStillPreservesInput)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vunsupported")),
        twitchSpec(QStringLiteral("alpha")),
    };
    auto multi = makeMulti(specs, {rumble, twitch});

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());
    input.setInputText(QStringLiteral("/not-a-real-provider-command value"));
    const auto commandBefore = input.getInputText();
    const auto commandFailure = input.handleSendMessage({});
    EXPECT_FALSE(commandFailure.isEmpty());
    EXPECT_EQ(input.getInputText(), commandBefore);
    EXPECT_TRUE(rumble->sent.empty());
    EXPECT_TRUE(twitch->sent.empty());
}

TEST_F(RumbleMultiChannelIntegration,
       PickerCreatedRumbleReplyPreservesVisibleTextExactlyOnce)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    rumble->context.writable = rumble->context.authenticated = true;
    auto twitch = std::make_shared<MutableChannel>(
        QStringLiteral("alpha"), MultiChannel::Platform::Twitch);
    twitch->context.writable = twitch->context.authenticated = true;
    const std::array specs{
        twitchSpec(QStringLiteral("alpha")),
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vpicker")),
    };
    auto multi = makeMulti(specs, {twitch, rumble});

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());
    auto reply = message(QStringLiteral("reply-id"), 1);
    input.setInputText(QStringLiteral("visible body"));
    input.setPickerReply(reply, rumble);
    input.setOverride(QStringLiteral("rumble"));
    const auto visible = input.getInputText();
    ASSERT_EQ(visible, QStringLiteral("@Viewer visible body "));

    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());

    EXPECT_THAT(rumble->sent, ::testing::ElementsAre(visible));
    EXPECT_TRUE(twitch->sent.empty());
    EXPECT_FALSE(input.hasReply());
    EXPECT_TRUE(input.getInputText().isEmpty());
}

TEST_F(RumbleMultiChannelIntegration,
       SingleChannelRumbleReplyUsesTheSameOrdinaryPath)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    rumble->context.writable = rumble->context.authenticated = true;

    Split split(nullptr);
    split.setChannel(IndirectChannel{rumble});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());
    auto reply = message(QStringLiteral("reply-id"), 1);
    input.setInputText(QStringLiteral("single body"));
    input.setPickerReply(reply, rumble);
    const auto visible = input.getInputText();

    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());

    EXPECT_THAT(rumble->sent, ::testing::ElementsAre(visible));
    EXPECT_FALSE(input.hasReply());
    EXPECT_TRUE(input.getInputText().isEmpty());
}

TEST_F(RumbleMultiChannelIntegration,
       RumbleReplyRateLimitAndTransportFailuresRetainOrdinaryDraftForRetry)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    rumble->context.writable = rumble->context.authenticated = true;
    rumble->deferSend = true;
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vretry"))};
    auto multi = makeMulti(specs, {rumble});

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());
    auto reply = message(QStringLiteral("reply-id"), 1);
    input.setInputText(QStringLiteral("retry body"));
    input.setPickerReply(reply, rumble);
    const auto visible = input.getInputText();

    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
    ASSERT_TRUE(rumble->pendingSend);
    EXPECT_FALSE(input.hasReply());
    auto failed = std::move(rumble->pendingSend);
    failed({Channel::SendOutcome::DefiniteFailure,
            QStringLiteral("rate limited")});
    EXPECT_EQ(input.getInputText(), visible);
    EXPECT_THAT(rumble->sent, ::testing::ElementsAre(visible));

    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
    ASSERT_TRUE(rumble->pendingSend);
    auto ambiguous = std::move(rumble->pendingSend);
    ambiguous({Channel::SendOutcome::Ambiguous,
               QStringLiteral("transport unavailable")});
    EXPECT_EQ(input.getInputText(), visible);
    EXPECT_THAT(rumble->sent, ::testing::ElementsAre(visible, visible));

    rumble->deferSend = false;
    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
    EXPECT_THAT(rumble->sent,
                ::testing::ElementsAre(visible, visible, visible));
    EXPECT_TRUE(input.getInputText().isEmpty());
}

TEST_F(RumbleMultiChannelIntegration,
       OfflineRumbleReplyBecomesOrdinaryWithoutDispatch)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    rumble->context.authenticated = true;
    rumble->context.writable = false;
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/voffline"))};
    auto multi = makeMulti(specs, {rumble});

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());
    auto reply = message(QStringLiteral("reply-id"), 1);
    input.setInputText(QStringLiteral("offline body"));
    input.setPickerReply(reply, rumble);
    const auto visible = input.getInputText();

    const auto failure = input.handleSendMessage({});
    EXPECT_TRUE(failure.contains(QStringLiteral("Message not sent")));
    EXPECT_FALSE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_EQ(input.getInputText(), visible);
    EXPECT_FALSE(input.hasReply());
    EXPECT_TRUE(rumble->sent.empty());
}

TEST_F(RumbleMultiChannelIntegration,
       RumbleAsyncOutcomesClearOnlyTheExactConfirmedDraft)
{
    auto rumble = std::make_shared<MutableChannel>(
        QStringLiteral("rumble-runtime"), MultiChannel::Platform::Rumble);
    rumble->context.writable = rumble->context.authenticated = true;
    rumble->deferSend = true;
    const std::array specs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vasync"))};
    auto multi = makeMulti(specs, {rumble});

    Split split(nullptr);
    split.setChannel(IndirectChannel{multi});
    TestableSplitInput input(nullptr, &split, &split.getChannelView());

    input.setInputText(QStringLiteral("confirmed"));
    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
    ASSERT_TRUE(rumble->pendingSend);
    auto confirmed = std::move(rumble->pendingSend);
    confirmed({Channel::SendOutcome::Confirmed, {}});
    EXPECT_TRUE(input.getInputText().isEmpty());

    for (const auto outcome : {Channel::SendOutcome::DefiniteFailure,
                               Channel::SendOutcome::Ambiguous})
    {
        input.setInputText(QStringLiteral("preserved"));
        EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
        ASSERT_TRUE(rumble->pendingSend);
        auto pending = std::move(rumble->pendingSend);
        pending({outcome, QStringLiteral("not confirmed")});
        EXPECT_EQ(input.getInputText(), QStringLiteral("preserved"));
    }

    input.setInputText(QStringLiteral("original"));
    EXPECT_TRUE(input.handleSendMessage({}).isEmpty());
    ASSERT_TRUE(rumble->pendingSend);
    input.setInputText(QStringLiteral("new draft"));
    auto stale = std::move(rumble->pendingSend);
    stale({Channel::SendOutcome::Confirmed, {}});
    EXPECT_EQ(input.getInputText(), QStringLiteral("new draft"));

    EXPECT_THAT(rumble->sent,
                ::testing::ElementsAre(QStringLiteral("confirmed"),
                                       QStringLiteral("preserved"),
                                       QStringLiteral("preserved"),
                                       QStringLiteral("original")));
}

TEST_F(RumbleMultiChannelIntegration,
       ReplacingAggregateDisconnectsOldCallbacks)
{
    auto dispatcher = std::make_shared<InlineDispatcher>();
    RumbleChannelProvider provider(dispatcher);
    auto oldCreated = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                           QStringLiteral("vold"));
    auto newCreated = provider.getOrCreate(RumbleChannelKeyKind::EmbedId,
                                           QStringLiteral("vnew"));
    ASSERT_TRUE(oldCreated);
    ASSERT_TRUE(newCreated);
    auto oldRuntime = *oldCreated;
    auto newRuntime = *newCreated;
    const std::array oldSpecs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vold"))};
    const std::array newSpecs{
        rumbleSpec(QStringLiteral("https://rumble.com/embed/vnew"))};
    auto oldMulti = makeMulti(oldSpecs, {oldRuntime});
    auto newMulti = makeMulti(newSpecs, {newRuntime});

    Split split(nullptr);
    split.setChannel(IndirectChannel{oldMulti});
    int tabRefreshes = 0;
    int liveRefreshes = 0;
    auto splitConnection = split.actionRequested.connect(
        [&](Split::Action action) {
            if (action == Split::Action::RefreshTab)
            {
                ++tabRefreshes;
            }
        });
    auto viewConnection = split.getChannelView().liveStatusChanged.connect([&] {
        ++liveRefreshes;
    });

    split.setChannel(IndirectChannel{newMulti});
    tabRefreshes = 0;
    liveRefreshes = 0;
    ASSERT_TRUE(oldRuntime->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(oldRuntime->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(tabRefreshes, 0);
    EXPECT_EQ(liveRefreshes, 0);

    ASSERT_TRUE(newRuntime->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(newRuntime->transitionTo(RumbleChannelState::Connected));
    EXPECT_EQ(tabRefreshes, 2);
    EXPECT_EQ(liveRefreshes, 1);
}
