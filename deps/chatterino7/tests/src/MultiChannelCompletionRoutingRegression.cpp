// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Emote.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageDraftCapability.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/LinkResolver.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/listview/GenericListItem.hpp"
#include "widgets/listview/GenericListModel.hpp"
#include "widgets/listview/GenericListView.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/Plugin.hpp"
#    include "controllers/plugins/PluginController.hpp"

#    include <lauxlib.h>
#    include <sol/state_view.hpp>
#endif

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QTextCursor>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#ifdef CHATTERINO_HAVE_PLUGINS
namespace chatterino {

// Plugin and PluginController deliberately expose this friend only to tests.
// Keep this definition in sync with the shared test accessor in Plugins.cpp.
class PluginControllerAccess
{
public:
    static bool tryLoadFromDir(const QDir &pluginDir)
    {
        return getApp()->getPlugins()->tryLoadFromDir(pluginDir);
    }

    static void openLibrariesFor(Plugin *plugin)
    {
        getApp()->getPlugins()->openLibrariesFor(plugin);
    }

    static std::map<QString, std::unique_ptr<Plugin>> &plugins()
    {
        return getApp()->getPlugins()->plugins_;
    }

    static lua_State *state(Plugin *pl)
    {
        return pl->state_;
    }
};

}  // namespace chatterino
#endif

namespace chatterino {
namespace {

class RegressionChannel final : public Channel
{
public:
    RegressionChannel(QString name, MultiChannel::Platform platform)
        : Channel(name, platform == MultiChannel::Platform::Twitch
                            ? Channel::Type::Twitch
                        : platform == MultiChannel::Platform::Kick
                            ? Channel::Type::Kick
                            : Channel::Type::Rumble)
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
        this->context.channelID = std::move(name);
        this->context.accountID =
            this->context.platform + QStringLiteral("-account");
    }

    bool canSendMessage() const override
    {
        return this->context.authenticated;
    }

    bool isWritable() const override
    {
        return this->context.writable;
    }

    bool isLive() const override
    {
        return this->live;
    }

    MessageSendContext messageSendContext() const override
    {
        ++this->contextReads;
        return this->context;
    }

    void sendMessage(const QString &message) override
    {
        this->sent.push_back(message);
        if (this->onSend)
        {
            this->onSend();
        }
    }

    MessageSendContext context;
    bool live = false;
    mutable size_t contextReads = 0;
    std::vector<QString> sent;
    std::function<void()> onSend;
};

class RegressionApplication final : public mock::BaseApplication
{
public:
    RegressionApplication()
        : windowManager(this->args_, this->paths_, this->settings, this->theme,
                        this->fonts)
        , commands(this->paths_)
#ifdef CHATTERINO_HAVE_PLUGINS
        , plugins(this->paths_)
#endif
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

    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &this->seventvPersonalEmotes;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    ILinkResolver *getLinkResolver() override
    {
        return &this->linkResolver;
    }

#ifdef CHATTERINO_HAVE_PLUGINS
    PluginController *getPlugins() override
    {
        return &this->plugins;
    }
#endif

    mock::MockTwitchIrcServer twitch;
    mock::EmptyLinkResolver linkResolver;
    mock::EmptyLogging logging;
    HotkeyController hotkeys;
    WindowManager windowManager;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
    BttvEmotes bttvEmotes;
    FfzEmotes ffzEmotes;
    SeventvEmotes seventvEmotes;
    SeventvPersonalEmotes seventvPersonalEmotes;
#ifdef CHATTERINO_HAVE_PLUGINS
    PluginController plugins;
#endif
};

class TestableSplitInput final : public SplitInput
{
public:
    using SplitInput::handleSendMessage;
    using SplitInput::insertCompletionText;
    using SplitInput::insertEmotePopupSelection;
    using SplitInput::setRoutingPlatformOverride;
    using SplitInput::SplitInput;

    MessageDraft draft() const
    {
        return this->currentMessageDraft(this->getInputText());
    }

    ResizingTextEdit *editor() const
    {
        return this->ui_.textEdit;
    }

    InputCompletionPopup *completionPopup() const
    {
        return this->inputCompletionPopup_.data();
    }

    PixmapButton *routingPlatformButton() const
    {
        return this->ui_.routingPlatformButton;
    }

    QWidget *automaticRoutingIndicator() const
    {
        return this->ui_.automaticRoutingIndicator;
    }

    void applyScale(float scale)
    {
        this->setScale(scale);
    }

    std::optional<QString> routingPlatformOverride() const
    {
        return this->routingPlatformOverride_;
    }

    std::optional<QString> effectiveRoutingPlatformOverride() const
    {
        return SplitInput::effectiveRoutingPlatformOverride();
    }

    bool draftRoutingOverrideActive() const
    {
        return this->draftRoutingOverrideActive_;
    }

    QLabel *routingTemporaryMarker() const
    {
        return this->ui_.routingTemporaryMarker;
    }
};

EmotePtr namedEmote(QString name, QString id)
{
    return std::shared_ptr<Emote>(new Emote{
        .name = EmoteName{name},
        .id = EmoteId{id},
    });
}

std::shared_ptr<EmoteMap> parsedBttvGlobal(QString name, QString id)
{
    QJsonArray input{
        QJsonObject{{QStringLiteral("id"), std::move(id)},
                    {QStringLiteral("code"), std::move(name)}},
    };
    const EmoteMap current;
    return std::make_shared<EmoteMap>(
        bttv::detail::parseGlobalEmotes(input, current));
}

std::shared_ptr<EmoteMap> parsedFfzGlobal(QString name, int id)
{
    const QJsonObject emote{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), std::move(name)},
        {QStringLiteral("owner"),
         QJsonObject{{QStringLiteral("display_name"),
                      QStringLiteral("catalog-owner")}}},
        {QStringLiteral("urls"),
         QJsonObject{{QStringLiteral("1"),
                      QStringLiteral("//cdn.example/ffz/1")}}},
        {QStringLiteral("width"), 28},
        {QStringLiteral("height"), 28},
    };
    const QJsonObject set{
        {QStringLiteral("id"), 1},
        {QStringLiteral("emoticons"), QJsonArray{emote}},
    };
    const QJsonObject input{
        {QStringLiteral("default_sets"), QJsonArray{1}},
        {QStringLiteral("sets"),
         QJsonObject{{QStringLiteral("1"), set}}},
    };
    return std::make_shared<EmoteMap>(
        ffz::detail::parseGlobalEmotes(input));
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

MessagePtr ingress(QString id, QString login,
                   MessageFlags flags = MessageFlags{})
{
    auto message = std::make_shared<Message>();
    message->id = std::move(id);
    message->loginName = std::move(login);
    message->flags = flags;
    return message;
}

class MultiChannelCompletionRoutingRegression : public ::testing::Test
{
protected:
    using Platform = MultiChannel::Platform;
    using Spec = MultiChannel::Spec;

    void SetUp() override
    {
        getSettings()->useSmartEmoteCompletion = false;
        getSettings()->emoteCompletionWithColon = true;
        getSettings()->prefixOnlyEmoteCompletion = true;

        this->split = std::make_unique<Split>(nullptr);
        this->input = std::make_unique<TestableSplitInput>(
            nullptr, this->split.get(), &this->split->getChannelView());
        this->install({twitch(QStringLiteral("alpha")),
                       twitch(QStringLiteral("beta")),
                       twitch(QStringLiteral("gamma"))});
    }

    void TearDown() override
    {
        this->input.reset();
        this->split.reset();
        this->multi.reset();
        this->registry.clear();
#ifdef CHATTERINO_HAVE_PLUGINS
        PluginControllerAccess::plugins().clear();
#endif
    }

    static Spec twitch(QString name)
    {
        return {.platform = Platform::Twitch, .name = std::move(name)};
    }

    static Spec kick(QString name)
    {
        return {.platform = Platform::Kick, .name = std::move(name)};
    }

    static Spec rumble(QString name)
    {
        return {
            .platform = Platform::Rumble,
            .name = QStringLiteral("https://rumble.com/c/%1").arg(name),
        };
    }

    static QString registryKey(const Spec &spec)
    {
        auto name = spec.name;
        if (spec.platform == Platform::Rumble &&
            !name.startsWith(QStringLiteral("https://")))
        {
            name = QStringLiteral("https://rumble.com/c/%1").arg(name);
        }
        return QString::number(static_cast<int>(spec.platform)) + QChar{0x1f} +
               name;
    }

    std::shared_ptr<RegressionChannel> ensure(const Spec &spec)
    {
        const auto key = registryKey(spec);
        auto channel = this->registry.value(key);
        if (!channel)
        {
            channel =
                std::make_shared<RegressionChannel>(spec.name, spec.platform);
            this->registry.insert(key, channel);
        }
        return channel;
    }

    std::shared_ptr<RegressionChannel> channel(
        const QString &name, Platform platform = Platform::Twitch) const
    {
        return this->registry.value(
            registryKey(Spec{.platform = platform, .name = name}));
    }

    void install(std::vector<Spec> specs)
    {
        for (const auto &spec : specs)
        {
            this->ensure(spec);
        }
        this->multi = std::make_shared<MultiChannel>(
            std::span{specs},
            MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
            [this](const Spec &spec) -> ChannelPtr {
                return this->registry.value(registryKey(spec));
            });
        this->split->setChannel(IndirectChannel{this->multi});
        QCoreApplication::processEvents();
    }

    void setLive(const std::shared_ptr<RegressionChannel> &channel, bool live)
    {
        ASSERT_NE(channel, nullptr);
        channel->live = live;
        this->multi->childStateChanged.invoke();
        QCoreApplication::processEvents();
    }

    QAction *routingAction(const QString &name) const
    {
        auto *menu = this->input->routingPlatformButton()->menu();
        if (menu == nullptr)
        {
            return nullptr;
        }
        const auto actions = menu->actions();
        const auto found = std::ranges::find(actions, name, &QAction::text);
        return found == actions.end() ? nullptr : *found;
    }

    static void setSendable(const std::shared_ptr<RegressionChannel> &channel,
                            bool sendable)
    {
        ASSERT_NE(channel, nullptr);
        channel->context.writable = sendable;
        channel->context.authenticated = sendable;
    }

    static void completeCapabilities(
        const std::shared_ptr<RegressionChannel> &channel)
    {
        ASSERT_NE(channel, nullptr);
        channel->context.emoteCapabilitiesComplete = true;
    }

    static void allow(
        const std::shared_ptr<RegressionChannel> &channel,
        const DraftEmoteCandidate &candidate,
        std::optional<DraftEmoteAvailability> availability = std::nullopt)
    {
        ASSERT_NE(channel, nullptr);
        channel->context.emoteCapabilities.push_back({
            .identity = candidate.identity,
            .insertionText = candidate.insertionText,
            .availability = availability.value_or(candidate.availability),
        });
    }

    static void allowMap(
        const std::shared_ptr<RegressionChannel> &channel,
        const EmoteMap &emotes, const QString &provider)
    {
        const DraftEmoteAvailability availability{
            .platform = QStringLiteral("twitch"),
        };
        ASSERT_NE(channel, nullptr);
        appendDraftEmoteCapabilities(channel->context.emoteCapabilities,
                                     emotes, provider, availability);
    }

    static DraftEmoteCandidate candidate(
        QString provider, QString id, QString text, QString platform,
        std::optional<QString> channelID = std::nullopt,
        std::optional<QString> accountID = std::nullopt)
    {
        return {
            .identity =
                {
                    .provider = std::move(provider),
                    .id = EmoteId{std::move(id)},
                },
            .insertionText = std::move(text),
            .availability =
                {
                    .platform = std::move(platform),
                    .channelID = std::move(channelID),
                    .accountID = std::move(accountID),
                },
        };
    }

    void seedBttv(const QString &name, const QString &id)
    {
        auto map = std::make_shared<EmoteMap>();
        map->insert({EmoteName{name}, namedEmote(name, id)});
        this->application.bttvEmotes.setEmotes(std::move(map));
    }

    void seedFfz(const QString &name, const QString &id)
    {
        auto map = std::make_shared<EmoteMap>();
        map->insert({EmoteName{name}, namedEmote(name, id)});
        this->application.ffzEmotes.setEmotes(std::move(map));
    }

    void seedSeventv(const QString &name, const QString &id)
    {
        auto map = std::make_shared<EmoteMap>();
        map->insert({EmoteName{name}, namedEmote(name, id)});
        this->application.seventvEmotes.setGlobalEmotes(std::move(map));
    }

    void configureMixedProviderOverlap(const QString &bttvName,
                                       const QString &seventvName)
    {
        this->seedBttv(bttvName, QStringLiteral("bttv-shared"));
        this->seedSeventv(seventvName, QStringLiteral("7tv-shared"));

        auto alpha = this->channel(QStringLiteral("alpha"));
        auto beta = this->channel(QStringLiteral("beta"));
        auto gamma = this->channel(QStringLiteral("gamma"));
        setSendable(alpha, true);
        setSendable(beta, true);
        setSendable(gamma, true);
        completeCapabilities(alpha);
        completeCapabilities(beta);
        completeCapabilities(gamma);

        const auto bttv =
            candidate(QStringLiteral("bttv"), QStringLiteral("bttv-shared"),
                      bttvName, QStringLiteral("twitch"));
        const auto seventv =
            candidate(QStringLiteral("7tv"), QStringLiteral("7tv-shared"),
                      seventvName, QStringLiteral("twitch"));
        allow(alpha, bttv);
        allow(beta, bttv);
        allow(beta, seventv);
        allow(gamma, seventv);
    }

    void chooseColonCompletion(const QString &query, int row = 0,
                               int *rowCount = nullptr)
    {
        this->input->insertText(QStringLiteral(":") + query);
        QCoreApplication::processEvents();

        auto *popup = this->input->completionPopup();
        ASSERT_NE(popup, nullptr);
        auto *view = popup->findChild<GenericListView *>();
        ASSERT_NE(view, nullptr);
        ASSERT_NE(view->model_, nullptr);
        if (rowCount != nullptr)
        {
            *rowCount = view->model_->rowCount();
        }
        ASSERT_GE(row, 0);
        ASSERT_LT(row, view->model_->rowCount());
        auto *item = GenericListItem::fromVariant(
            view->model_->data(view->model_->index(row), Qt::DisplayRole));
        ASSERT_NE(item, nullptr);
        item->action();
        QCoreApplication::processEvents();
    }

    void pick(const DraftEmoteCandidate &selected)
    {
        Link link{Link::InsertText, selected.insertionText};
        std::optional<DraftEmoteCandidate> value{selected};
        this->input->insertEmotePopupSelection(link, value, false);
    }

    QString send()
    {
        return this->input->handleSendMessage({});
    }

    size_t totalSends() const
    {
        size_t result = 0;
        for (const auto &entry : this->registry)
        {
            result += entry->sent.size();
        }
        return result;
    }

    void expectOnlySend(const QString &name, const QString &text,
                        Platform platform = Platform::Twitch) const
    {
        EXPECT_EQ(this->totalSends(), 1U);
        const auto destination = this->channel(name, platform);
        ASSERT_NE(destination, nullptr);
        EXPECT_THAT(destination->sent, ::testing::ElementsAre(text));
        EXPECT_TRUE(this->input->getInputText().isEmpty());
        const auto *active = this->multi->activeChannel();
        if (active && active->channel != destination)
        {
            const auto feedback = this->multi->getLastMessage();
            EXPECT_TRUE(feedback == nullptr ||
                        !feedback->messageText.contains(
                            QStringLiteral("Sent via")));
        }
    }

    static void append(
        const std::shared_ptr<RegressionChannel> &channel,
        const MessagePtr &message,
        std::optional<MessageFlags> overridingFlags = std::nullopt)
    {
        ASSERT_NE(channel, nullptr);
        auto copy = message;
        channel->messageAppended.invoke(copy, std::move(overridingFlags));
    }

    uint64_t activity(const QString &name,
                      Platform platform = Platform::Twitch) const
    {
        const auto expected = this->channel(name, platform);
        for (const auto &child : this->multi->channels())
        {
            if (child.channel == expected)
            {
                return child.activitySequence;
            }
        }
        return 0;
    }

#ifdef CHATTERINO_HAVE_PLUGINS
    void registerCollisionPlugin(const QString &completion)
    {
        PluginMeta meta;
        meta.name = QStringLiteral("Routing regression");
        meta.license = QStringLiteral("MIT");
        meta.description = QStringLiteral("In-memory completion collision");

        QDir directory(this->application.paths_.pluginsDirectory);
        directory = QDir(
            directory.absoluteFilePath(QStringLiteral("routing-regression")));
        ASSERT_TRUE(directory.mkpath(QStringLiteral(".")));

        auto plugin =
            std::make_unique<Plugin>(QStringLiteral("routing-regression"),
                                     luaL_newstate(), meta, directory);
        auto *raw = plugin.get();
        PluginControllerAccess::plugins().insert(
            {QStringLiteral("routing-regression"), std::move(plugin)});
        PluginControllerAccess::openLibrariesFor(raw);

        sol::state_view lua(PluginControllerAccess::state(raw));
        lua["collision_completion"] = completion.toStdString();
        lua.script(R"lua(
            c2.register_callback(
                c2.EventType.CompletionRequested,
                function(_)
                    return {
                        hide_others = false,
                        values = {_G.collision_completion},
                    }
                end
            )
        )lua");
    }
#endif

    void runPopupCompletion(bool smart, const QString &name, const QString &id)
    {
        getSettings()->useSmartEmoteCompletion = smart;
        this->seedBttv(name, id);

        auto alpha = this->channel(QStringLiteral("alpha"));
        auto beta = this->channel(QStringLiteral("beta"));
        setSendable(alpha, true);
        setSendable(beta, true);
        completeCapabilities(alpha);
        completeCapabilities(beta);
        const auto selected = candidate(QStringLiteral("bttv"), id, name,
                                        QStringLiteral("twitch"));
        allow(beta, selected);

        this->chooseColonCompletion(name);
        const auto draft = this->input->draft();
        ASSERT_TRUE(draft.provenanceValid);
        ASSERT_EQ(draft.emotes.size(), 1U);
        EXPECT_EQ(draft.emotes[0].identity.provider, QStringLiteral("bttv"));
        EXPECT_EQ(draft.emotes[0].identity.id.string, id);

        EXPECT_TRUE(this->send().isEmpty());
        this->expectOnlySend(QStringLiteral("beta"), name + QChar{' '});
        EXPECT_TRUE(this->input->getInputText().isEmpty());
    }

    RegressionApplication application;
    QHash<QString, std::shared_ptr<RegressionChannel>> registry;
    std::unique_ptr<Split> split;
    std::unique_ptr<TestableSplitInput> input;
    std::shared_ptr<MultiChannel> multi;
};

TEST_F(MultiChannelCompletionRoutingRegression,
       PlainCompatibleActiveChildReceivesExactlyOneSend)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    append(beta, ingress(QStringLiteral("beta-new"), QStringLiteral("viewer")));
    this->input->setInputText(QStringLiteral("plain active"));

    EXPECT_TRUE(this->send().isEmpty());

    this->expectOnlySend(QStringLiteral("alpha"),
                         QStringLiteral("plain active"));
    EXPECT_TRUE(this->input->getInputText().isEmpty());
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ActiveIncompatibleUsesNewestLiveCompatibleFallbackOnce)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);
    append(beta, ingress(QStringLiteral("beta-1"), QStringLiteral("one")));
    append(gamma, ingress(QStringLiteral("gamma-1"), QStringLiteral("two")));
    append(beta, ingress(QStringLiteral("beta-2"), QStringLiteral("three")));
    this->input->setInputText(QStringLiteral("newest fallback"));

    EXPECT_TRUE(this->send().isEmpty());

    this->expectOnlySend(QStringLiteral("beta"),
                         QStringLiteral("newest fallback"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       EqualActivityUsesStableCurrentChildOrder)
{
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("beta")), true);
    setSendable(this->channel(QStringLiteral("gamma")), true);
    this->input->setInputText(QStringLiteral("stable tie"));

    EXPECT_TRUE(this->send().isEmpty());

    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("stable tie"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       AutomaticIndicatorAndSubmissionShareTheBestThreePlatformRoute)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta")),
                   rumble(QStringLiteral("gamma"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    setSendable(alpha, true);
    setSendable(beta, true);
    setSendable(gamma, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    completeCapabilities(gamma);
    this->setLive(gamma, true);
    const auto kickOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("kick-only"),
                  QStringLiteral("KickOnly"), QStringLiteral("kick"));
    allow(beta, kickOnly);

    this->input->setInputText(QStringLiteral("KickOnly"));
    QCoreApplication::processEvents();

    auto *button = this->input->routingPlatformButton();
    auto *indicator = this->input->automaticRoutingIndicator();
    ASSERT_NE(button, nullptr);
    ASSERT_NE(indicator, nullptr);
    EXPECT_FALSE(button->isHidden());
    EXPECT_FALSE(button->pixmap().isNull());
    EXPECT_FALSE(button->borderColor().isValid());
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Kick")))
        << button->toolTip().toStdString();
    EXPECT_TRUE(button->accessibleName().contains(QStringLiteral("Kick")));
    EXPECT_FALSE(indicator->isHidden());
    EXPECT_TRUE(indicator->toolTip().contains(QStringLiteral("Kick")));
    EXPECT_TRUE(indicator->toolTip().contains(
        QStringLiteral("best available destination")));
    EXPECT_EQ(indicator->accessibleName(), indicator->toolTip());

    this->input->setInputText(QStringLiteral("ordinary words"));
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Twitch")));
    EXPECT_TRUE(indicator->isHidden());
    this->input->setInputText(QStringLiteral("KickOnly"));
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Kick")));
    EXPECT_FALSE(indicator->isHidden());

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("KickOnly"),
                         Platform::Kick);
    EXPECT_TRUE(indicator->isHidden());
}

TEST_F(MultiChannelCompletionRoutingRegression,
       CompletionRefreshesAutomaticIndicatorUsingSelectedIdentity)
{
    this->install(
        {twitch(QStringLiteral("alpha")), kick(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);

    const auto name = QStringLiteral("RouteCollision");
    const auto primary = candidate(
        QStringLiteral("bttv"), QStringLiteral("primary-same"), name,
        QStringLiteral("twitch"), QStringLiteral("alpha"));
    const auto selected = candidate(
        QStringLiteral("7tv"), QStringLiteral("kick-same"), name,
        QStringLiteral("kick"), QStringLiteral("beta"));
    allow(alpha, primary);
    allow(beta, selected);
    this->seedSeventv(name, selected.identity.id.string);

    this->chooseColonCompletion(name);
    const auto draft = this->input->draft();
    ASSERT_TRUE(draft.provenanceValid);
    ASSERT_EQ(draft.emotes.size(), 1U);
    EXPECT_EQ(draft.emotes[0].identity, selected.identity);

    auto *button = this->input->routingPlatformButton();
    auto *indicator = this->input->automaticRoutingIndicator();
    ASSERT_NE(button, nullptr);
    ASSERT_NE(indicator, nullptr);
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Kick")))
        << button->toolTip().toStdString();
    EXPECT_FALSE(indicator->isHidden());
    EXPECT_TRUE(indicator->toolTip().contains(QStringLiteral("Kick")));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), name + QChar{' '},
                         Platform::Kick);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       AutomaticIndicatorUsesCurrentThemeTextColor)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);
    this->multi->childStateChanged.invoke();
    QCoreApplication::processEvents();

    auto *indicator = this->input->automaticRoutingIndicator();
    ASSERT_NE(indicator, nullptr);
    ASSERT_FALSE(indicator->isHidden());

    for (const auto &themeName :
         {QStringLiteral("Dark"), QStringLiteral("Light")})
    {
        this->application.theme.themeName.setValue(themeName);
        this->application.theme.update();
        QCoreApplication::processEvents();

        const auto foreground = indicator->palette().color(QPalette::Text);
        EXPECT_EQ(foreground, this->application.theme.splits.input.text);
        EXPECT_NE(foreground, this->application.theme.splits.input.background);
    }
}

TEST_F(MultiChannelCompletionRoutingRegression,
       OfflineFirstRumbleUsesEligibleChildWithoutChangingLayoutOrDraft)
{
    this->install({rumble(QStringLiteral("alpha")),
                   twitch(QStringLiteral("beta")),
                   kick(QStringLiteral("gamma"))});
    auto alpha = this->channel(QStringLiteral("alpha"), Platform::Rumble);
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Kick);
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);
    this->setLive(alpha, false);

    auto *button = this->input->routingPlatformButton();
    auto *indicator = this->input->automaticRoutingIndicator();
    ASSERT_NE(button, nullptr);
    ASSERT_NE(indicator, nullptr);
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Twitch")))
        << button->toolTip().toStdString();
    EXPECT_FALSE(indicator->isHidden());
    EXPECT_TRUE(indicator->accessibleName().contains(
        QStringLiteral("Twitch")));

    const auto draft = QStringLiteral("preserved automatic draft");
    this->input->setInputText(draft);
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Twitch")))
        << button->toolTip().toStdString();
    EXPECT_EQ(this->multi->activeChannelIndex(), 0U);

    setSendable(alpha, true);
    this->setLive(alpha, true);
    EXPECT_EQ(this->input->getInputText(), draft);
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Rumble")))
        << button->toolTip().toStdString();
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_EQ(this->multi->activeChannelIndex(), 0U);

    setSendable(alpha, false);
    this->setLive(alpha, false);
    EXPECT_EQ(this->input->getInputText(), draft);
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Twitch")))
        << button->toolTip().toStdString();
    EXPECT_FALSE(indicator->isHidden());

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), draft);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       MultipleRumbleChildrenUseStableEligibleProviderOrder)
{
    this->install(
        {rumble(QStringLiteral("alpha")), rumble(QStringLiteral("beta")),
         twitch(QStringLiteral("gamma")), kick(QStringLiteral("delta"))});
    auto alpha = this->channel(QStringLiteral("alpha"), Platform::Rumble);
    auto beta = this->channel(QStringLiteral("beta"), Platform::Rumble);
    auto gamma = this->channel(QStringLiteral("gamma"));
    auto delta = this->channel(QStringLiteral("delta"), Platform::Kick);
    setSendable(alpha, false);
    setSendable(beta, true);
    setSendable(gamma, true);
    setSendable(delta, true);
    this->setLive(alpha, false);
    this->setLive(beta, true);

    auto *button = this->input->routingPlatformButton();
    ASSERT_NE(button, nullptr);
    this->input->setInputText(QStringLiteral("second rumble"));
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Rumble")))
        << button->toolTip().toStdString();
    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_THAT(beta->sent,
                ::testing::ElementsAre(QStringLiteral("second rumble")));
    EXPECT_TRUE(alpha->sent.empty());
    EXPECT_TRUE(gamma->sent.empty());
    EXPECT_TRUE(delta->sent.empty());

    setSendable(beta, false);
    this->setLive(beta, false);
    this->input->setInputText(QStringLiteral("offline twitch"));
    EXPECT_FALSE(gamma->isLive());
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Twitch")))
        << button->toolTip().toStdString();
    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_THAT(gamma->sent,
                ::testing::ElementsAre(QStringLiteral("offline twitch")));
    EXPECT_TRUE(delta->sent.empty());

    setSendable(gamma, false);
    this->multi->childStateChanged.invoke();
    QCoreApplication::processEvents();
    this->input->setInputText(QStringLiteral("offline kick"));
    EXPECT_FALSE(delta->isLive());
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Kick")))
        << button->toolTip().toStdString();
    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_THAT(delta->sent,
                ::testing::ElementsAre(QStringLiteral("offline kick")));
    EXPECT_EQ(this->totalSends(), 3U);
    EXPECT_EQ(this->multi->activeChannelIndex(), 0U);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       NoEligibleAutomaticDestinationIsStableAndRejectsWithoutDispatch)
{
    this->install(
        {rumble(QStringLiteral("alpha")), twitch(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"), Platform::Rumble);
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, false);
    this->setLive(alpha, false);

    const auto draft = QStringLiteral("must remain available for correction");
    this->input->setInputText(draft);
    auto *button = this->input->routingPlatformButton();
    auto *indicator = this->input->automaticRoutingIndicator();
    ASSERT_NE(button, nullptr);
    ASSERT_NE(indicator, nullptr);
    EXPECT_TRUE(button->pixmap().isNull());
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_FALSE(button->borderColor().isValid());
    EXPECT_EQ(button->toolTip(),
              QStringLiteral("No multi-channel destination is available"));
    EXPECT_EQ(button->accessibleName(), button->toolTip());

    const auto failure = this->send();
    EXPECT_FALSE(failure.isEmpty());
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), draft);

    this->multi->childStateChanged.invoke();
    QCoreApplication::processEvents();
    EXPECT_TRUE(button->pixmap().isNull());
    EXPECT_EQ(button->toolTip(),
              QStringLiteral("No multi-channel destination is available"));
    EXPECT_EQ(this->input->getInputText(), draft);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       OpenCompletionPopupRefreshesWhenEligibilityChanges)
{
    this->install(
        {rumble(QStringLiteral("alpha")), twitch(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"), Platform::Rumble);
    auto beta = this->channel(QStringLiteral("beta"));
    const auto rumbleOnly =
        candidate(QStringLiteral("bttv"), QStringLiteral("route-rumble"),
                  QStringLiteral("RouteRumble"), QStringLiteral("rumble"));
    const auto twitchOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("route-twitch"),
                  QStringLiteral("RouteTwitch"), QStringLiteral("twitch"));
    this->seedBttv(rumbleOnly.insertionText, rumbleOnly.identity.id.string);
    this->seedSeventv(twitchOnly.insertionText, twitchOnly.identity.id.string);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    allow(alpha, rumbleOnly);
    allow(beta, twitchOnly);
    setSendable(alpha, false);
    setSendable(beta, true);
    this->setLive(alpha, false);

    this->input->insertText(QStringLiteral(":Route"));
    QCoreApplication::processEvents();
    auto *popup = this->input->completionPopup();
    ASSERT_NE(popup, nullptr);
    auto *view = popup->findChild<GenericListView *>();
    ASSERT_NE(view, nullptr);
    ASSERT_NE(view->model_, nullptr);
    ASSERT_EQ(view->model_->rowCount(), 2);
    const auto readsBefore = alpha->contextReads + beta->contextReads;
    const auto draftBefore = this->input->getInputText();

    setSendable(alpha, true);
    this->setLive(alpha, true);
    ASSERT_EQ(view->model_->rowCount(), 2);
    EXPECT_GT(alpha->contextReads + beta->contextReads, readsBefore);
    EXPECT_EQ(this->input->getInputText(), draftBefore);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       CompletionPriorityUsesCurrentEligibleAutomaticPrimary)
{
    this->install(
        {rumble(QStringLiteral("alpha")), twitch(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"), Platform::Rumble);
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);
    this->setLive(alpha, false);
    this->seedBttv(QStringLiteral("RouteBttv"), QStringLiteral("route-bttv"));
    this->seedSeventv(QStringLiteral("RouteSeventv"),
                      QStringLiteral("route-seventv"));

    completion::EmoteSource source(
        this->multi.get(),
        std::make_unique<completion::ClassicEmoteStrategy>());
    source.setInputContext(
        MessageDraft::fromPlainText(QStringLiteral(":Route")));
    source.update(QStringLiteral(":Route"));

    ASSERT_EQ(source.output().size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(source.output(), [](const auto &item) {
        return item.primaryChannelCompatible;
    }));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ExplicitOverrideWinsOverEmotesAndUsesPackagedPlatformBadge)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta")),
                   rumble(QStringLiteral("gamma"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    setSendable(alpha, true);
    setSendable(beta, true);
    setSendable(gamma, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    completeCapabilities(gamma);
    this->setLive(gamma, true);
    const auto kickOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("kick-only"),
                  QStringLiteral("KickOnly"), QStringLiteral("kick"));
    allow(beta, kickOnly);

    this->input->insertText(QStringLiteral(":kick"));
    this->input->insertCompletionText(kickOnly.insertionText, kickOnly, false);
    ASSERT_EQ(this->input->draft().emotes.size(), 1U);
    this->input->setRoutingPlatformOverride(QStringLiteral("rumble"));
    QCoreApplication::processEvents();

    const auto overridden = this->input->draft();
    ASSERT_TRUE(overridden.destinationPlatformOverride);
    EXPECT_EQ(*overridden.destinationPlatformOverride,
              QStringLiteral("rumble"));
    auto *button = this->input->routingPlatformButton();
    auto *indicator = this->input->automaticRoutingIndicator();
    ASSERT_NE(button, nullptr);
    ASSERT_NE(indicator, nullptr);
    EXPECT_FALSE(button->pixmap().isNull());
    EXPECT_TRUE(button->borderColor().isValid());
    EXPECT_TRUE(indicator->isHidden());
    EXPECT_TRUE(button->toolTip().contains(QStringLiteral("Rumble")));
    EXPECT_TRUE(button->accessibleName().contains(QStringLiteral("override"),
                                                  Qt::CaseInsensitive));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("gamma"), QStringLiteral("KickOnly "),
                         Platform::Rumble);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       RumbleOverrideTracksInitialAndOfflineToLiveAvailability)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta")),
                   rumble(QStringLiteral("gamma"))});
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(gamma, true);

    auto *twitchAction = this->routingAction(QStringLiteral("Twitch"));
    auto *kickAction = this->routingAction(QStringLiteral("Kick"));
    auto *rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(twitchAction, nullptr);
    ASSERT_NE(kickAction, nullptr);
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_TRUE(twitchAction->isEnabled());
    EXPECT_TRUE(kickAction->isEnabled());
    EXPECT_FALSE(rumbleAction->isEnabled());
    EXPECT_TRUE(rumbleAction->toolTip().contains(
        QStringLiteral("live chat session"), Qt::CaseInsensitive));
    this->input->setRoutingPlatformOverride(QStringLiteral("rumble"));
    EXPECT_FALSE(this->input->routingPlatformOverride());

    this->setLive(gamma, true);
    rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_TRUE(rumbleAction->isEnabled());
    rumbleAction->trigger();
    ASSERT_TRUE(this->input->routingPlatformOverride());
    EXPECT_EQ(*this->input->routingPlatformOverride(),
              QStringLiteral("rumble"));
    EXPECT_FALSE(this->input->routingPlatformButton()->pixmap().isNull());
}

TEST_F(MultiChannelCompletionRoutingRegression,
       LastLiveRumbleGoingOfflineClearsOverrideAndPreservesActiveDraft)
{
    this->install(
        {twitch(QStringLiteral("alpha")), rumble(QStringLiteral("gamma"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    setSendable(alpha, true);
    setSendable(gamma, true);
    this->setLive(gamma, true);
    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input->setReply(reply, alpha);
    this->input->setInputText(QStringLiteral("draft survives transition"));
    this->input->setRoutingPlatformOverride(QStringLiteral("rumble"));
    ASSERT_TRUE(this->input->routingPlatformOverride());
    ASSERT_TRUE(this->input->routingPlatformButton()->borderColor().isValid());

    this->setLive(gamma, false);

    EXPECT_FALSE(this->input->routingPlatformOverride());
    EXPECT_FALSE(this->input->draft().destinationPlatformOverride);
    EXPECT_EQ(this->input->getInputText(),
              QStringLiteral("draft survives transition"));
    EXPECT_FALSE(this->input->routingPlatformButton()->borderColor().isValid());
    EXPECT_TRUE(this->input->routingPlatformButton()->toolTip().contains(
        QStringLiteral("Automatic")));
    auto *rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_FALSE(rumbleAction->isEnabled());

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("alpha"),
                         QStringLiteral("draft survives transition"),
                         Platform::Twitch);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       AnyLiveRumbleKeepsOverrideAvailableAcrossSamePlatformChildren)
{
    this->install({twitch(QStringLiteral("alpha")),
                   rumble(QStringLiteral("gamma")),
                   rumble(QStringLiteral("delta"))});
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    auto delta = this->channel(QStringLiteral("delta"), Platform::Rumble);
    setSendable(gamma, true);
    setSendable(delta, true);
    this->setLive(delta, true);

    auto *rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_TRUE(rumbleAction->isEnabled());
    this->input->setRoutingPlatformOverride(QStringLiteral("rumble"));
    ASSERT_TRUE(this->input->routingPlatformOverride());

    this->setLive(gamma, false);
    EXPECT_TRUE(this->input->routingPlatformOverride());
    rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_TRUE(rumbleAction->isEnabled());

    this->setLive(delta, false);
    EXPECT_FALSE(this->input->routingPlatformOverride());
    rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_FALSE(rumbleAction->isEnabled());
}

TEST_F(MultiChannelCompletionRoutingRegression,
       LoggedOutLiveRumbleRemainsAnOverrideAndUsesExistingSendFeedback)
{
    this->install(
        {twitch(QStringLiteral("alpha")), rumble(QStringLiteral("gamma"))});
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(gamma, false);
    this->setLive(gamma, true);

    auto *rumbleAction = this->routingAction(QStringLiteral("Rumble"));
    ASSERT_NE(rumbleAction, nullptr);
    EXPECT_TRUE(rumbleAction->isEnabled());
    rumbleAction->trigger();
    this->input->setInputText(QStringLiteral("requires authentication"));

    ASSERT_TRUE(this->input->routingPlatformOverride());
    EXPECT_FALSE(this->send().isEmpty());
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(),
              QStringLiteral("requires authentication"));
    EXPECT_TRUE(this->input->routingPlatformOverride());
}

TEST_F(MultiChannelCompletionRoutingRegression,
       OverrideUsesActivityAndChildOrderOnlyWithinSelectedPlatform)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta")),
                   kick(QStringLiteral("gamma"))});
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(this->channel(QStringLiteral("beta"), Platform::Kick), true);
    setSendable(this->channel(QStringLiteral("gamma"), Platform::Kick), true);
    append(this->channel(QStringLiteral("gamma"), Platform::Kick),
           ingress(QStringLiteral("gamma-new"), QStringLiteral("viewer")));

    this->input->setRoutingPlatformOverride(QStringLiteral("kick"));
    this->input->setInputText(QStringLiteral("forced platform"));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("gamma"),
                         QStringLiteral("forced platform"), Platform::Kick);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       UnavailableOverrideRetainsDraftAndNeverEscapes)
{
    this->install(
        {twitch(QStringLiteral("alpha")), kick(QStringLiteral("beta"))});
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(this->channel(QStringLiteral("beta"), Platform::Kick), false);
    this->input->setRoutingPlatformOverride(QStringLiteral("kick"));
    this->input->setInputText(QStringLiteral("keep this draft"));
    QCoreApplication::processEvents();

    EXPECT_TRUE(this->input->routingPlatformButton()->toolTip().contains(
        QStringLiteral("unavailable")));
    EXPECT_FALSE(this->send().isEmpty());
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), QStringLiteral("keep this draft"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       OverrideIsPerInputAndCanReturnToAutomatic)
{
    this->install(
        {twitch(QStringLiteral("alpha")), kick(QStringLiteral("beta"))});
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(this->channel(QStringLiteral("beta"), Platform::Kick), true);
    auto other = std::make_unique<TestableSplitInput>(
        nullptr, this->split.get(), &this->split->getChannelView());

    auto *menu = this->input->routingPlatformButton()->menu();
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(this->input->routingPlatformButton()->focusPolicy(),
              Qt::StrongFocus);
    sendKey(*this->input->routingPlatformButton(), Qt::Key_Space);
    EXPECT_TRUE(menu->isVisible());
    menu->hide();
    const auto actions = menu->actions();
    auto automatic = std::ranges::find(
        actions, QStringLiteral("Automatic routing"), &QAction::text);
    auto kick =
        std::ranges::find(actions, QStringLiteral("Kick"), &QAction::text);
    ASSERT_NE(automatic, actions.end());
    ASSERT_NE(kick, actions.end());
    EXPECT_FALSE((*kick)->icon().isNull());
    EXPECT_FALSE((*kick)->toolTip().isEmpty());
    (*kick)->trigger();

    ASSERT_TRUE(this->input->draft().destinationPlatformOverride);
    EXPECT_FALSE(other->draft().destinationPlatformOverride);
    EXPECT_TRUE(this->input->routingPlatformButton()->borderColor().isValid());
    EXPECT_FALSE(other->routingPlatformButton()->borderColor().isValid());
    const auto normalWidth = this->input->routingPlatformButton()->width();
    this->input->applyScale(2.F);
    EXPECT_GT(this->input->routingPlatformButton()->width(), normalWidth);
    EXPECT_TRUE(this->input->routingPlatformButton()->borderColor().isValid());

    menu = this->input->routingPlatformButton()->menu();
    ASSERT_NE(menu, nullptr);
    const auto resetActions = menu->actions();
    automatic = std::ranges::find(
        resetActions, QStringLiteral("Automatic routing"), &QAction::text);
    ASSERT_NE(automatic, resetActions.end());
    (*automatic)->trigger();
    EXPECT_FALSE(this->input->draft().destinationPlatformOverride);
    EXPECT_FALSE(this->input->routingPlatformButton()->borderColor().isValid());
    EXPECT_TRUE(this->input->routingPlatformButton()->toolTip().contains(
        QStringLiteral("Twitch")));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TabCyclesAvailablePlatformsAndRetainsEditorFocus)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta")),
                   rumble(QStringLiteral("gamma"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    auto gamma = this->channel(QStringLiteral("gamma"), Platform::Rumble);
    setSendable(alpha, true);
    setSendable(beta, true);
    setSendable(gamma, true);
    this->setLive(gamma, true);

    this->input->setWindowFlag(Qt::Window);
    this->split->show();
    this->input->show();
    this->split->activateWindow();
    this->input->setInputText(QStringLiteral("cycle me"));
    this->input->editor()->setFocus();
    QCoreApplication::processEvents();
    sendKey(*this->input->editor(), Qt::Key_Tab);
    ASSERT_TRUE(this->input->effectiveRoutingPlatformOverride());
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("twitch"));
    EXPECT_TRUE(this->input->draftRoutingOverrideActive());
    EXPECT_FALSE(this->input->routingTemporaryMarker()->isHidden());

    sendKey(*this->input->editor(), Qt::Key_Tab);
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("kick"));
    sendKey(*this->input->editor(), Qt::Key_Tab);
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("rumble"));
    sendKey(*this->input->editor(), Qt::Key_Tab);
    EXPECT_FALSE(this->input->effectiveRoutingPlatformOverride());
    EXPECT_TRUE(this->input->draftRoutingOverrideActive());

    sendKey(*this->input->editor(), Qt::Key_Tab);
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("twitch"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TabOverridePreservesPersistentChoiceAndClearsOnlyAfterSuccessfulSend)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    setSendable(alpha, true);
    setSendable(beta, true);

    this->input->setRoutingPlatformOverride(QStringLiteral("kick"));
    this->input->setInputText(QStringLiteral("temporary route"));
    this->input->editor()->setFocus();
    sendKey(*this->input->editor(), Qt::Key_Tab);
    ASSERT_TRUE(this->input->effectiveRoutingPlatformOverride());
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("twitch"));
    EXPECT_EQ(*this->input->routingPlatformOverride(), QStringLiteral("kick"));

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_FALSE(this->input->draftRoutingOverrideActive());
    ASSERT_TRUE(this->input->effectiveRoutingPlatformOverride());
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("kick"));
    this->expectOnlySend(QStringLiteral("alpha"),
                         QStringLiteral("temporary route"), Platform::Twitch);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       RejectedTabOverridePreservesDraftAndTemporaryChoice)
{
    this->install({twitch(QStringLiteral("alpha")),
                   kick(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    setSendable(alpha, true);
    setSendable(beta, true);
    this->input->setInputText(QStringLiteral("keep this"));
    this->input->editor()->setFocus();
    sendKey(*this->input->editor(), Qt::Key_Tab);
    ASSERT_TRUE(this->input->draftRoutingOverrideActive());
    setSendable(alpha, false);

    EXPECT_FALSE(this->send().isEmpty());
    EXPECT_EQ(this->input->getInputText(), QStringLiteral("keep this"));
    EXPECT_TRUE(this->input->draftRoutingOverrideActive());
    ASSERT_TRUE(this->input->effectiveRoutingPlatformOverride());
    EXPECT_EQ(*this->input->effectiveRoutingPlatformOverride(),
              QStringLiteral("twitch"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ClassicCompletionCarriesIdentityThroughSubmission)
{
    this->runPopupCompletion(false, QStringLiteral("ClassicOnly"),
                             QStringLiteral("classic-id"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       SmartCompletionCarriesIdentityThroughSubmission)
{
    this->runPopupCompletion(true, QStringLiteral("SmartOnly"),
                             QStringLiteral("smart-id"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ChannelScopedEmoteRoutesAcrossMixedTwitchAndKickChildren)
{
    this->install(
        {twitch(QStringLiteral("alpha")), kick(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"), Platform::Kick);
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto selected =
        candidate(QStringLiteral("7tv"), QStringLiteral("kick-channel-id"),
                  QStringLiteral("KickOnly"), QStringLiteral("kick"),
                  QStringLiteral("beta"));
    allow(beta, selected);

    this->input->insertText(QStringLiteral(":kick"));
    this->input->insertCompletionText(selected.insertionText, selected, false);
    ASSERT_EQ(this->input->draft().emotes.size(), 1U);

    EXPECT_TRUE(this->send().isEmpty());

    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("KickOnly "),
                         Platform::Kick);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       SameTextProviderCollisionUsesTheActuallySelectedIdentity)
{
    const auto name = QStringLiteral("SameFace");
    this->seedBttv(name, QStringLiteral("bttv-same"));
    this->seedFfz(name, QStringLiteral("ffz-same"));

    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto bttv =
        candidate(QStringLiteral("bttv"), QStringLiteral("bttv-same"), name,
                  QStringLiteral("twitch"));
    const auto ffz =
        candidate(QStringLiteral("ffz"), QStringLiteral("ffz-same"), name,
                  QStringLiteral("twitch"));
    allow(alpha, bttv,
          DraftEmoteAvailability{
              .platform = QStringLiteral("twitch"),
              .channelID = QStringLiteral("alpha"),
          });
    allow(beta, ffz,
          DraftEmoteAvailability{
              .platform = QStringLiteral("twitch"),
              .channelID = QStringLiteral("beta"),
          });

    int firstRowCount = 0;
    this->chooseColonCompletion(name, 0, &firstRowCount);
    ASSERT_EQ(firstRowCount, 2);
    const auto firstDraft = this->input->draft();
    ASSERT_TRUE(firstDraft.provenanceValid);
    ASSERT_EQ(firstDraft.emotes.size(), 1U);
    const auto firstIdentity = firstDraft.emotes[0].identity;
    ASSERT_TRUE(firstIdentity == bttv.identity ||
                firstIdentity == ffz.identity);

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_EQ(this->totalSends(), 1U);
    if (firstIdentity == bttv.identity)
    {
        EXPECT_THAT(alpha->sent,
                    ::testing::ElementsAre(name + QChar{' '}));
        EXPECT_TRUE(beta->sent.empty());
    }
    else
    {
        EXPECT_TRUE(alpha->sent.empty());
        EXPECT_THAT(beta->sent,
                    ::testing::ElementsAre(name + QChar{' '}));
    }
    EXPECT_TRUE(this->input->getInputText().isEmpty());

    int secondRowCount = 0;
    this->chooseColonCompletion(name, 1, &secondRowCount);
    ASSERT_EQ(secondRowCount, 2);
    const auto secondDraft = this->input->draft();
    ASSERT_TRUE(secondDraft.provenanceValid);
    ASSERT_EQ(secondDraft.emotes.size(), 1U);
    const auto secondIdentity = secondDraft.emotes[0].identity;

    const auto selectedBothProviders =
        (firstIdentity == bttv.identity && secondIdentity == ffz.identity) ||
        (firstIdentity == ffz.identity && secondIdentity == bttv.identity);
    ASSERT_TRUE(selectedBothProviders);

    const auto alphaSendsBeforeSecond = alpha->sent.size();
    const auto betaSendsBeforeSecond = beta->sent.size();
    EXPECT_TRUE(this->send().isEmpty());

    EXPECT_EQ(this->totalSends(), 2U);
    if (secondIdentity == bttv.identity)
    {
        EXPECT_EQ(alpha->sent.size(), alphaSendsBeforeSecond + 1);
        EXPECT_EQ(beta->sent.size(), betaSendsBeforeSecond);
    }
    else
    {
        EXPECT_EQ(alpha->sent.size(), alphaSendsBeforeSecond);
        EXPECT_EQ(beta->sent.size(), betaSendsBeforeSecond + 1);
    }
    EXPECT_THAT(alpha->sent, ::testing::ElementsAre(name + QChar{' '}));
    EXPECT_THAT(beta->sent, ::testing::ElementsAre(name + QChar{' '}));
    EXPECT_TRUE(this->channel(QStringLiteral("gamma"))->sent.empty());
    EXPECT_TRUE(this->input->getInputText().isEmpty());
}

TEST_F(MultiChannelCompletionRoutingRegression,
       IdenticalTokenEditDropsOnlyTheOverlappingSelectedRange)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);

    const auto first =
        candidate(QStringLiteral("bttv"), QStringLiteral("first-twin"),
                  QStringLiteral("Twin"), QStringLiteral("twitch"));
    const auto second =
        candidate(QStringLiteral("ffz"), QStringLiteral("second-twin"),
                  QStringLiteral("Twin"), QStringLiteral("twitch"));
    allow(alpha, first);
    allow(beta, second);

    this->input->insertText(QStringLiteral(":first"));
    this->input->insertCompletionText(first.insertionText, first, false);
    this->input->insertText(QStringLiteral(":second"));
    this->input->insertCompletionText(second.insertionText, second, false);
    ASSERT_EQ(this->input->getInputText(), QStringLiteral("Twin Twin "));
    ASSERT_EQ(this->input->draft().emotes.size(), 2U);

    QTextCursor cursor(this->input->editor()->document());
    cursor.setPosition(0);
    cursor.setPosition(first.insertionText.size(), QTextCursor::KeepAnchor);
    cursor.insertText(first.insertionText);
    QCoreApplication::processEvents();

    const auto edited = this->input->draft();
    ASSERT_TRUE(edited.provenanceValid);
    ASSERT_EQ(edited.emotes.size(), 1U);
    EXPECT_EQ(edited.emotes[0].start, 5);
    EXPECT_EQ(edited.emotes[0].identity.provider, QStringLiteral("ffz"));
    EXPECT_EQ(edited.emotes[0].identity.id.string,
              QStringLiteral("second-twin"));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("Twin Twin "));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       MultipleScopedEmotesWithoutIntersectionRejectStably)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto alphaOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("alpha-only"),
                  QStringLiteral("AlphaOnly"), QStringLiteral("twitch"),
                  QStringLiteral("alpha"));
    const auto betaOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("beta-only"),
                  QStringLiteral("BetaOnly"), QStringLiteral("twitch"),
                  QStringLiteral("beta"));
    allow(alpha, alphaOnly);
    allow(beta, betaOnly);

    this->input->insertText(QStringLiteral(":alpha"));
    this->input->insertCompletionText(alphaOnly.insertionText, alphaOnly,
                                      false);
    this->input->insertText(QStringLiteral(":beta"));
    this->input->insertCompletionText(betaOnly.insertionText, betaOnly, false);
    const auto before = this->input->getInputText();
    const auto beforeDraft = this->input->draft();

    const auto firstFailure = this->send();
    const auto secondFailure = this->send();

    EXPECT_FALSE(firstFailure.isEmpty());
    EXPECT_EQ(secondFailure, firstFailure);
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), before);
    const auto afterDraft = this->input->draft();
    EXPECT_TRUE(afterDraft.provenanceValid);
    ASSERT_EQ(afterDraft.emotes.size(), 2U);
    EXPECT_EQ(afterDraft.emotes[0].identity, beforeDraft.emotes[0].identity);
    EXPECT_EQ(afterDraft.emotes[1].identity, beforeDraft.emotes[1].identity);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ProductionProviderObjectsRetainStableRoutingIds)
{
    const auto bttv = parsedBttvGlobal(
        QStringLiteral("CatalogBttv"), QStringLiteral("bttv-catalog-id"));
    const auto ffz = parsedFfzGlobal(QStringLiteral("CatalogFfz"), 930001);
    const auto twitchEmote =
        this->application.emotes.getTwitchEmotes()->getOrCreateEmote(
            EmoteId{QStringLiteral("twitch-catalog-id")},
            EmoteName{QStringLiteral("CatalogTwitch")});

    ASSERT_EQ(bttv->size(), 1U);
    ASSERT_EQ(ffz->size(), 1U);
    ASSERT_TRUE(twitchEmote);
    EXPECT_EQ(bttv->begin()->second->id.string,
              QStringLiteral("bttv-catalog-id"));
    EXPECT_EQ(ffz->begin()->second->id.string, QStringLiteral("930001"));
    EXPECT_EQ(twitchEmote->id.string,
              QStringLiteral("twitch-catalog-id"));

    EmoteMap twitchMap;
    twitchMap.emplace(twitchEmote->name, twitchEmote);
    std::vector<DraftEmoteCapability> capabilities;
    const DraftEmoteAvailability platformScope{
        .platform = QStringLiteral("twitch"),
    };
    appendDraftEmoteCapabilities(capabilities, *bttv,
                                 QStringLiteral("bttv"), platformScope);
    appendDraftEmoteCapabilities(capabilities, *ffz, QStringLiteral("ffz"),
                                 platformScope);
    appendDraftEmoteCapabilities(capabilities, twitchMap,
                                 QStringLiteral("twitch"), platformScope);

    ASSERT_EQ(capabilities.size(), 3U);
    for (const auto &capability : capabilities)
    {
        EXPECT_FALSE(capability.identity.id.string.isEmpty());
    }
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ProductionCatalogObjectsPopulateAggregateCompletionAndContexts)
{
    const auto bttv = parsedBttvGlobal(
        QStringLiteral("AggregateBttv"), QStringLiteral("aggregate-bttv"));
    const auto ffz =
        parsedFfzGlobal(QStringLiteral("AggregateFfz"), 930002);
    const auto twitchEmote =
        this->application.emotes.getTwitchEmotes()->getOrCreateEmote(
            EmoteId{QStringLiteral("aggregate-twitch")},
            EmoteName{QStringLiteral("AggregateTwitch")});
    auto twitchMap = std::make_shared<EmoteMap>();
    twitchMap->emplace(twitchEmote->name, twitchEmote);

    this->application.bttvEmotes.setEmotes(bttv);
    this->application.ffzEmotes.setEmotes(ffz);
    this->application.accounts.twitch.getCurrent()->setEmotes(twitchMap);

    auto alpha = std::make_shared<TwitchChannel>(QStringLiteral("alpha"));
    auto beta = std::make_shared<TwitchChannel>(QStringLiteral("beta"));
    const std::array specs{twitch(QStringLiteral("alpha")),
                           twitch(QStringLiteral("beta"))};
    MultiChannel aggregate(
        std::span{specs},
        MultiChannelIndicatorMode::PlatformBadgeIfUnselected,
        [alpha, beta](const Spec &spec) -> ChannelPtr {
            return spec.name == QStringLiteral("alpha") ? alpha : beta;
        });
    completion::EmoteSource source(
        &aggregate,
        std::make_unique<completion::ClassicTabEmoteStrategy>());

    const auto assertResolved = [&source](const EmotePtr &emote,
                                          const QString &name,
                                          const QString &provider,
                                          const QString &id) {
        const auto resolution = source.resolveCandidate(emote, name);
        ASSERT_FALSE(resolution.ambiguous);
        ASSERT_TRUE(resolution.candidate);
        EXPECT_EQ(resolution.candidate->identity.provider, provider);
        EXPECT_EQ(resolution.candidate->identity.id.string, id);
    };
    assertResolved(bttv->begin()->second, QStringLiteral("AggregateBttv"),
                   QStringLiteral("bttv"), QStringLiteral("aggregate-bttv"));
    assertResolved(ffz->begin()->second, QStringLiteral("AggregateFfz"),
                   QStringLiteral("ffz"), QStringLiteral("930002"));
    assertResolved(twitchEmote, QStringLiteral("AggregateTwitch"),
                   QStringLiteral("twitch"),
                   QStringLiteral("aggregate-twitch"));

    const auto context = alpha->messageSendContext();
    const auto hasIdentity = [&context](const QString &provider,
                                        const QString &id) {
        return std::ranges::any_of(
            context.emoteCapabilities, [&](const auto &capability) {
                return capability.identity.provider == provider &&
                       capability.identity.id.string == id;
            });
    };
    EXPECT_TRUE(hasIdentity(QStringLiteral("bttv"),
                            QStringLiteral("aggregate-bttv")));
    EXPECT_TRUE(
        hasIdentity(QStringLiteral("ffz"), QStringLiteral("930002")));
    EXPECT_TRUE(hasIdentity(QStringLiteral("twitch"),
                            QStringLiteral("aggregate-twitch")));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ProductionParsedBttvAndSeventvCompletionsRouteSharedChild)
{
    const auto bttvName = QStringLiteral("ParsedBttvShared");
    const auto seventvName = QStringLiteral("ParsedSeventvShared");
    const auto bttv =
        parsedBttvGlobal(bttvName, QStringLiteral("parsed-bttv-shared"));
    auto seventv = std::make_shared<EmoteMap>();
    seventv->emplace(EmoteName{seventvName},
                     namedEmote(seventvName,
                                QStringLiteral("parsed-7tv-shared")));
    this->application.bttvEmotes.setEmotes(bttv);
    this->application.seventvEmotes.setGlobalEmotes(seventv);

    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    for (const auto &channel : {alpha, beta, gamma})
    {
        setSendable(channel, true);
        completeCapabilities(channel);
    }
    allowMap(alpha, *bttv, QStringLiteral("bttv"));
    allowMap(beta, *bttv, QStringLiteral("bttv"));
    allowMap(beta, *seventv, QStringLiteral("7tv"));
    allowMap(gamma, *seventv, QStringLiteral("7tv"));

    this->chooseColonCompletion(bttvName);
    this->chooseColonCompletion(seventvName);
    ASSERT_TRUE(this->input->draft().provenanceValid);
    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"),
                         bttvName + QChar{' '} + seventvName + QChar{' '});
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ProductionParsedFfzAndSeventvTypedUnionRoutesSharedChild)
{
    const auto ffzName = QStringLiteral("ParsedFfzShared");
    const auto seventvName = QStringLiteral("TypedParsedSeventvShared");
    const auto ffz = parsedFfzGlobal(ffzName, 930003);
    auto seventv = std::make_shared<EmoteMap>();
    seventv->emplace(EmoteName{seventvName},
                     namedEmote(seventvName,
                                QStringLiteral("typed-parsed-7tv")));

    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    for (const auto &channel : {alpha, beta, gamma})
    {
        setSendable(channel, true);
        completeCapabilities(channel);
    }
    allowMap(alpha, *seventv, QStringLiteral("7tv"));
    allowMap(beta, *ffz, QStringLiteral("ffz"));
    allowMap(beta, *seventv, QStringLiteral("7tv"));
    allowMap(gamma, *ffz, QStringLiteral("ffz"));

    const auto text = ffzName + QChar{' '} + seventvName;
    this->input->setInputText(text);
    ASSERT_TRUE(this->input->draft().emotes.empty());
    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), text);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       MixedProviderCompletionsRouteToTheirSharedChild)
{
    const auto bttvName = QStringLiteral("BttvShared");
    const auto seventvName = QStringLiteral("SeventvShared");
    this->configureMixedProviderOverlap(bttvName, seventvName);

    this->chooseColonCompletion(bttvName);
    this->chooseColonCompletion(seventvName);

    const auto draft = this->input->draft();
    ASSERT_TRUE(draft.provenanceValid);
    ASSERT_EQ(draft.emotes.size(), 2U);
    EXPECT_EQ(draft.emotes[0].identity.provider, QStringLiteral("bttv"));
    EXPECT_EQ(draft.emotes[1].identity.provider, QStringLiteral("7tv"));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"),
                         bttvName + QChar{' '} + seventvName + QChar{' '});
}

TEST_F(MultiChannelCompletionRoutingRegression,
       MixedProviderTabCompletionsRouteToTheirSharedChild)
{
    const auto bttvName = QStringLiteral("BttvShared");
    const auto seventvName = QStringLiteral("SeventvShared");
    this->configureMixedProviderOverlap(bttvName, seventvName);

    this->chooseColonCompletion(bttvName);
    this->chooseColonCompletion(seventvName);

    const auto draft = this->input->draft();
    ASSERT_TRUE(draft.provenanceValid);
    ASSERT_EQ(draft.emotes.size(), 2U);
    EXPECT_EQ(draft.emotes[0].identity.provider, QStringLiteral("bttv"));
    EXPECT_EQ(draft.emotes[1].identity.provider, QStringLiteral("7tv"));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"),
                         bttvName + QChar{' '} + seventvName + QChar{' '});
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TypedMixedProviderNamesUseTheUnionDictionary)
{
    const auto bttvName = QStringLiteral("TypedBttvShared");
    const auto seventvName = QStringLiteral("TypedSeventvShared");
    this->configureMixedProviderOverlap(bttvName, seventvName);

    this->input->setInputText(bttvName + QChar{' '} + seventvName);
    ASSERT_TRUE(this->input->draft().emotes.empty());

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"),
                         bttvName + QChar{' '} + seventvName);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TypedNonActiveProviderNameRoutesByItself)
{
    const auto bttvName = QStringLiteral("UnusedBttv");
    const auto seventvName = QStringLiteral("TypedSeventvOnly");
    this->configureMixedProviderOverlap(bttvName, seventvName);

    this->input->setInputText(seventvName);
    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), seventvName);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TypedMixedProvidersWithNoSharedChildUsePrimaryOnSupportTie)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, true);
    setSendable(beta, true);
    setSendable(gamma, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    completeCapabilities(gamma);

    const auto alphaOnly =
        candidate(QStringLiteral("bttv"), QStringLiteral("typed-alpha"),
                  QStringLiteral("TypedAlpha"), QStringLiteral("twitch"),
                  QStringLiteral("alpha"));
    const auto betaOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("typed-beta"),
                  QStringLiteral("TypedBeta"), QStringLiteral("twitch"),
                  QStringLiteral("beta"));
    allow(alpha, alphaOnly);
    allow(beta, betaOnly);

    const auto text =
        alphaOnly.insertionText + QChar{' '} + betaOnly.insertionText;
    this->input->setInputText(text);

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("alpha"), text);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TypedEmoteOccurrencesRouteToDestinationSupportingMost)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, true);
    setSendable(beta, true);
    setSendable(gamma, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    completeCapabilities(gamma);

    const auto alphaOnly =
        candidate(QStringLiteral("bttv"), QStringLiteral("count-alpha"),
                  QStringLiteral("CountAlpha"), QStringLiteral("twitch"),
                  QStringLiteral("alpha"));
    const auto betaOnly =
        candidate(QStringLiteral("7tv"), QStringLiteral("count-beta"),
                  QStringLiteral("CountBeta"), QStringLiteral("twitch"),
                  QStringLiteral("beta"));
    allow(alpha, alphaOnly);
    allow(beta, betaOnly);

    const auto text = alphaOnly.insertionText + QChar{' '} +
                      betaOnly.insertionText + QChar{' '} +
                      betaOnly.insertionText;
    this->input->setInputText(text);

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), text);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       FullSevenTvUrlSendsUnchangedAsPlainLink)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);

    const auto url =
        QStringLiteral("https://7tv.app/emotes/01H0123456789ABCDEFGHJKMNP");
    const auto misleadingCapability =
        candidate(QStringLiteral("7tv"), QStringLiteral("url-collision"), url,
                  QStringLiteral("twitch"), QStringLiteral("beta"));
    allow(beta, misleadingCapability);
    this->input->setInputText(url);

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("alpha"), url);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       SameTextProviderOverlapTabCompletionUsesIdentityUnion)
{
    const auto name = QStringLiteral("ProviderOverlap");
    this->seedBttv(name, QStringLiteral("bttv-overlap"));
    this->seedSeventv(name, QStringLiteral("7tv-overlap"));

    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));
    setSendable(alpha, true);
    setSendable(beta, true);
    setSendable(gamma, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    completeCapabilities(gamma);
    allow(alpha,
          candidate(QStringLiteral("bttv"),
                    QStringLiteral("bttv-overlap"), name,
                    QStringLiteral("twitch")));
    allow(beta,
          candidate(QStringLiteral("7tv"), QStringLiteral("7tv-overlap"),
                    name, QStringLiteral("twitch")));

    this->chooseColonCompletion(name);

    const auto draft = this->input->draft();
    ASSERT_TRUE(draft.provenanceValid);
    ASSERT_EQ(draft.emotes.size(), 1U);

    EXPECT_TRUE(this->send().isEmpty());
    EXPECT_EQ(alpha->sent.size() + beta->sent.size(), 1U);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       PickerIdentityControlsOnlyItsInsertedSameTextRange)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto selected =
        candidate(QStringLiteral("7tv"), QStringLiteral("picker-beta"),
                  QStringLiteral("Same"), QStringLiteral("twitch"),
                  QStringLiteral("beta"));
    allow(beta, selected);

    this->input->insertText(QStringLiteral("Same "));
    this->pick(selected);
    ASSERT_EQ(this->input->getInputText(), QStringLiteral("Same Same "));
    const auto draft = this->input->draft();
    ASSERT_TRUE(draft.provenanceValid);
    ASSERT_EQ(draft.emotes.size(), 1U);
    EXPECT_EQ(draft.emotes[0].start, 5);
    EXPECT_EQ(draft.emotes[0].identity.id.string,
              QStringLiteral("picker-beta"));

    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("Same Same "));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       TypedPluginCollisionRemainsPlainText)
{
#ifndef CHATTERINO_HAVE_PLUGINS
    GTEST_SKIP() << "This build has no plugin completion layer";
#else
    const auto name = QStringLiteral("Collision");
    this->seedBttv(name, QStringLiteral("native-collision"));
    this->registerCollisionPlugin(name + QChar{' '});
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    allow(alpha,
          candidate(QStringLiteral("bttv"), QStringLiteral("native-collision"),
                    name, QStringLiteral("twitch")));

    this->input->insertText(name);
    const auto draft = this->input->draft();
    EXPECT_TRUE(draft.emotes.empty());
    EXPECT_TRUE(this->send().isEmpty());
    this->expectOnlySend(QStringLiteral("alpha"), name);
#endif
}

TEST_F(MultiChannelCompletionRoutingRegression,
       AccountChangeBeforeSubmitUsesCurrentContext)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    alpha->context.accountID = QStringLiteral("old-account");
    beta->context.accountID = QStringLiteral("old-account");
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto selected =
        candidate(QStringLiteral("7tv"), QStringLiteral("personal"),
                  QStringLiteral("Personal"), QStringLiteral("twitch"),
                  std::nullopt, QStringLiteral("old-account"));
    allow(alpha, selected);
    allow(beta, selected);
    this->input->insertText(QStringLiteral(":personal"));
    this->input->insertCompletionText(selected.insertionText, selected, false);

    alpha->context.accountID = QStringLiteral("new-account");
    const auto alphaReads = alpha->contextReads;
    const auto betaReads = beta->contextReads;
    EXPECT_TRUE(this->send().isEmpty());

    EXPECT_GT(alpha->contextReads, alphaReads);
    EXPECT_GT(beta->contextReads, betaReads);
    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("Personal "));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       DisconnectBeforeSubmitUsesCurrentAuthentication)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    this->input->setInputText(QStringLiteral("disconnect reroute"));

    alpha->context.authenticated = false;
    const auto alphaReads = alpha->contextReads;
    const auto betaReads = beta->contextReads;
    EXPECT_TRUE(this->send().isEmpty());

    EXPECT_GT(alpha->contextReads, alphaReads);
    EXPECT_GT(beta->contextReads, betaReads);
    this->expectOnlySend(QStringLiteral("beta"),
                         QStringLiteral("disconnect reroute"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       EmoteRemovalBeforeSubmitUsesCurrentCapabilities)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto selected = candidate(
        QStringLiteral("ffz"), QStringLiteral("removed-before-submit"),
        QStringLiteral("Mutable"), QStringLiteral("twitch"));
    allow(alpha, selected);
    allow(beta, selected);
    this->input->insertText(QStringLiteral(":mutable"));
    this->input->insertCompletionText(selected.insertionText, selected, false);

    alpha->context.emoteCapabilities.clear();
    const auto alphaReads = alpha->contextReads;
    const auto betaReads = beta->contextReads;
    EXPECT_TRUE(this->send().isEmpty());

    EXPECT_GT(alpha->contextReads, alphaReads);
    EXPECT_GT(beta->contextReads, betaReads);
    this->expectOnlySend(QStringLiteral("beta"), QStringLiteral("Mutable "));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       SyntacticProviderCommandNeverFallsBack)
{
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("beta")), true);
    this->input->setInputText(
        QStringLiteral("/not-a-real-provider-command value"));
    const auto before = this->input->getInputText();

    const auto failure = this->send();
    const auto repeatedFailure = this->send();

    EXPECT_FALSE(failure.isEmpty());
    EXPECT_EQ(repeatedFailure, failure);
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), before);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ProviderCommandCannotEscapeAnOverride)
{
    this->install(
        {twitch(QStringLiteral("alpha")), kick(QStringLiteral("beta"))});
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(this->channel(QStringLiteral("beta"), Platform::Kick), true);
    this->input->setRoutingPlatformOverride(QStringLiteral("kick"));
    this->input->setInputText(
        QStringLiteral("/not-a-real-provider-command value"));
    const auto before = this->input->getInputText();

    EXPECT_FALSE(this->send().isEmpty());
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), before);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ReplyNeverFallsBackFromItsBoundChild)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, false);
    setSendable(beta, true);
    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input->setReply(reply, alpha);
    this->input->insertText(QStringLiteral("reply body"));
    const auto before = this->input->getInputText();

    const auto failure = this->send();
    const auto repeatedFailure = this->send();

    EXPECT_TRUE(failure.contains(QStringLiteral("Reply not sent")));
    EXPECT_EQ(repeatedFailure, failure);
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), before);
}

TEST_F(MultiChannelCompletionRoutingRegression, ReplyCannotEscapeAnOverride)
{
    this->install(
        {twitch(QStringLiteral("alpha")), kick(QStringLiteral("beta"))});
    auto alpha = this->channel(QStringLiteral("alpha"));
    setSendable(alpha, true);
    setSendable(this->channel(QStringLiteral("beta"), Platform::Kick), true);
    this->multi->setActiveChannelIndex(1);
    auto reply = std::make_shared<Message>();
    reply->id = QStringLiteral("reply-id");
    reply->displayName = QStringLiteral("viewer");
    this->input->setReply(reply, alpha);
    this->input->insertText(QStringLiteral("reply body"));
    EXPECT_TRUE(this->input->routingPlatformButton()->toolTip().contains(
        QStringLiteral("Twitch")));
    this->input->setRoutingPlatformOverride(QStringLiteral("kick"));
    EXPECT_TRUE(this->input->routingPlatformButton()->toolTip().contains(
        QStringLiteral("Kick")));
    const auto before = this->input->getInputText();

    const auto failure = this->send();

    EXPECT_TRUE(failure.contains(QStringLiteral("override")));
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), before);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       RemovedChildInvalidatesASelectedScopedEmote)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    setSendable(alpha, true);
    setSendable(beta, true);
    completeCapabilities(alpha);
    completeCapabilities(beta);
    const auto selected =
        candidate(QStringLiteral("7tv"), QStringLiteral("removed-child"),
                  QStringLiteral("RemovedChild"), QStringLiteral("twitch"),
                  QStringLiteral("beta"));
    allow(beta, selected);
    this->input->insertText(QStringLiteral(":removed"));
    this->input->insertCompletionText(selected.insertionText, selected, false);
    const auto before = this->input->getInputText();

    this->install(
        {twitch(QStringLiteral("alpha")), twitch(QStringLiteral("gamma"))});
    setSendable(this->channel(QStringLiteral("alpha")), true);
    setSendable(this->channel(QStringLiteral("gamma")), true);
    completeCapabilities(this->channel(QStringLiteral("gamma")));
    const auto failure = this->send();
    const auto repeatedFailure = this->send();

    EXPECT_FALSE(failure.isEmpty());
    EXPECT_EQ(repeatedFailure, failure);
    EXPECT_EQ(this->totalSends(), 0U);
    EXPECT_EQ(this->input->getInputText(), before);
    ASSERT_EQ(this->input->draft().emotes.size(), 1U);
    EXPECT_EQ(this->input->draft().emotes[0].identity.id.string,
              QStringLiteral("removed-child"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ReconstructedMembershipUsesAddedChildAndNewStableOrder)
{
    this->install(
        {twitch(QStringLiteral("alpha")), twitch(QStringLiteral("beta"))});
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("beta")), true);
    this->input->setInputText(QStringLiteral("new membership"));

    this->install({twitch(QStringLiteral("alpha")),
                   twitch(QStringLiteral("delta")),
                   twitch(QStringLiteral("beta"))});
    setSendable(this->channel(QStringLiteral("alpha")), false);
    setSendable(this->channel(QStringLiteral("delta")), true);
    setSendable(this->channel(QStringLiteral("beta")), true);
    EXPECT_TRUE(this->send().isEmpty());

    this->expectOnlySend(QStringLiteral("delta"),
                         QStringLiteral("new membership"));
}

TEST_F(MultiChannelCompletionRoutingRegression,
       LocalHistoryAndSystemMessagesDoNotAdvanceActivity)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));

    append(alpha, ingress({}, QStringLiteral("local")));
    append(beta, ingress(QStringLiteral("history"), QStringLiteral("viewer"),
                         MessageFlags{MessageFlag::RecentMessage}));
    append(gamma, ingress(QStringLiteral("system"), QStringLiteral("system"),
                          MessageFlags{MessageFlag::System}));
    append(gamma,
           ingress(QStringLiteral("overridden"), QStringLiteral("viewer")),
           MessageFlags{MessageFlag::DoNotLog});

    EXPECT_EQ(this->activity(QStringLiteral("alpha")), 0U);
    EXPECT_EQ(this->activity(QStringLiteral("beta")), 0U);
    EXPECT_EQ(this->activity(QStringLiteral("gamma")), 0U);
}

TEST_F(MultiChannelCompletionRoutingRegression,
       ProviderLiveMessagesAdvanceActivityDeterministically)
{
    auto alpha = this->channel(QStringLiteral("alpha"));
    auto beta = this->channel(QStringLiteral("beta"));
    auto gamma = this->channel(QStringLiteral("gamma"));

    append(alpha, ingress(QStringLiteral("alpha-1"), QStringLiteral("one")));
    append(beta, ingress(QStringLiteral("beta-1"), QStringLiteral("two")));
    append(alpha, ingress(QStringLiteral("alpha-1"), QStringLiteral("one")));
    append(gamma, ingress(QStringLiteral("gamma-1"), QStringLiteral("three")));

    EXPECT_EQ(this->activity(QStringLiteral("alpha")), 1U);
    EXPECT_EQ(this->activity(QStringLiteral("beta")), 2U);
    EXPECT_EQ(this->activity(QStringLiteral("gamma")), 3U);
}

}  // namespace
}  // namespace chatterino
