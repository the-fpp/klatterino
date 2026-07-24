// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Channel.hpp"
#include "controllers/completion/sources/EmoteSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "providers/rumble/RumbleApi.hpp"
#include "providers/rumble/RumbleEvent.hpp"
#include "providers/rumble/RumbleMessageBuilder.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace chatterino {
namespace {

using rumble::EventParser;
using rumble::EventState;
using rumble::InitEvent;
using rumble::MessageDto;
using rumble::MessagesEvent;
using rumble::ResolvedBadge;

QJsonObject messageObject(QString id, QString text = QStringLiteral("hello"))
{
    return {
        {QStringLiteral("id"), std::move(id)},
        {QStringLiteral("user_id"), QStringLiteral("u")},
        {QStringLiteral("channel_id"), QStringLiteral("c")},
        {QStringLiteral("text"), std::move(text)},
        {QStringLiteral("created_on"), QStringLiteral("2026-01-01T00:00:00Z")},
    };
}

QByteArray eventJson(const QString &type, QJsonObject data)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("type"), type},
                             {QStringLiteral("data"), std::move(data)},
                         })
        .toJson(QJsonDocument::Compact);
}

QDateTime timestamp()
{
    return QDateTime::fromString(QStringLiteral("2026-01-01T00:00:00Z"),
                                 Qt::ISODate)
        .toUTC();
}

const TextElement *textElement(const MessagePtr &message, std::size_t index)
{
    if (index >= message->elements.size())
    {
        ADD_FAILURE() << "message element index is out of bounds";
        return nullptr;
    }
    const auto *element =
        dynamic_cast<const TextElement *>(message->elements[index].get());
    EXPECT_NE(element, nullptr);
    return element;
}

class AssetApplication final : public mock::BaseApplication
{
public:
    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    mock::EmoteController emotes;
};

class CountingTransport final : public rumble::Transport
{
public:
    std::unique_ptr<rumble::TransportHandle> start(
        rumble::TransportRequest, rumble::TransportCallbacks) override
    {
        ++this->starts;
        return nullptr;
    }

    int starts = 0;
};

TEST(RumbleBadge, ParsesOnlyAcceptedCatalogAndMembershipShapes)
{
    const auto hostileTitle = QStringLiteral("<b title=\"owner\">A&B</b>");
    QJsonObject catalog{
        {QStringLiteral("configured"),
         QJsonObject{
             {QStringLiteral("label"),
              QJsonObject{
                  {QStringLiteral("en"), QStringLiteral("Viewer")},
              }},
             {QStringLiteral("icons"),
              QJsonObject{
                  {QStringLiteral("48"),
                   QStringLiteral("/i/badges/viewer_48.png")},
                  {QStringLiteral("96"),
                   QStringLiteral("/i/badges/viewer_96.png")},
              }},
         }},
        {QStringLiteral("missing-title"), QJsonObject{}},
        {QStringLiteral("empty-title"),
         QJsonObject{{QStringLiteral("title"), QString{}}}},
        {QStringLiteral("wrong-title"),
         QJsonObject{
             {QStringLiteral("title"), 17},
             {QStringLiteral("icons"),
              QJsonObject{
                  {QStringLiteral("48"),
                   QStringLiteral("https://attacker.invalid/badge.png")}}},
         }},
        {QStringLiteral("hostile"),
         QJsonObject{{QStringLiteral("title"), hostileTitle}}},
        {QStringLiteral("invalid-definition"), false},
        {QString{}, QJsonObject{{QStringLiteral("title"),
                                 QStringLiteral("empty identity")}}},
    };
    auto message = messageObject(QStringLiteral("m1"));
    message.insert(
        QStringLiteral("badges"),
        QJsonArray{QStringLiteral("configured"), 7, QString{},
                   QStringLiteral("unknown"), QStringLiteral("missing-title"),
                   QStringLiteral("empty-title"), QStringLiteral("wrong-title"),
                   QStringLiteral("hostile")});
    message.insert(
        QStringLiteral("roles"),
        QJsonArray{QStringLiteral("broadcaster"), QStringLiteral("moderator")});

    EventParser parser;
    const auto parsed = parser.parse(
        eventJson(QStringLiteral("init"),
                  QJsonObject{
                      {QStringLiteral("config"),
                       QJsonObject{{QStringLiteral("badges"), catalog}}},
                      {QStringLiteral("messages"), QJsonArray{message}},
                  }));
    ASSERT_TRUE(parsed.event);
    ASSERT_TRUE(std::holds_alternative<InitEvent>(*parsed.event));
    const auto &init = std::get<InitEvent>(*parsed.event);
    EXPECT_EQ(init.badges.size(), 5);
    EXPECT_EQ(init.badges.value(QStringLiteral("configured")).title,
              QStringLiteral("Viewer"));
    EXPECT_EQ(init.badges.value(QStringLiteral("configured")).iconUrl48,
              QStringLiteral("https://rumble.com/i/badges/viewer_48.png"));
    EXPECT_EQ(init.badges.value(QStringLiteral("configured")).iconUrl96,
              QStringLiteral("https://rumble.com/i/badges/viewer_96.png"));
    EXPECT_EQ(init.badges.value(QStringLiteral("missing-title")).title,
              QStringLiteral("missing-title"));
    EXPECT_EQ(init.badges.value(QStringLiteral("empty-title")).title,
              QStringLiteral("empty-title"));
    EXPECT_EQ(init.badges.value(QStringLiteral("wrong-title")).title,
              QStringLiteral("wrong-title"));
    EXPECT_TRUE(
        init.badges.value(QStringLiteral("wrong-title")).iconUrl48.isEmpty());
    EXPECT_EQ(init.badges.value(QStringLiteral("hostile")).title, hostileTitle);
    EXPECT_FALSE(init.badges.contains(QStringLiteral("invalid-definition")));
    EXPECT_FALSE(init.badges.contains(QString{}));
    EXPECT_FALSE(parsed.diagnostics.empty());

    EventState state;
    auto output = state.process(*parsed.event, QStringLiteral("stream-a"));
    ASSERT_EQ(output.messages.size(), 1U);
    const auto &hydrated = output.messages.front();
    ASSERT_TRUE(hydrated.badgeIds);
    EXPECT_EQ(
        *hydrated.badgeIds,
        (QStringList{
            QStringLiteral("configured"), QString{}, QStringLiteral("unknown"),
            QStringLiteral("missing-title"), QStringLiteral("empty-title"),
            QStringLiteral("wrong-title"), QStringLiteral("hostile")}));
    ASSERT_EQ(hydrated.resolvedBadges.size(), 6U);
    EXPECT_EQ(
        hydrated.resolvedBadges[0],
        (ResolvedBadge{.id = QStringLiteral("configured"),
                       .title = QStringLiteral("Viewer"),
                       .iconUrl48 = QStringLiteral(
                           "https://rumble.com/i/badges/viewer_48.png"),
                       .iconUrl96 = QStringLiteral(
                           "https://rumble.com/i/badges/viewer_96.png")}));
    EXPECT_EQ(hydrated.resolvedBadges[1],
              (ResolvedBadge{.id = QStringLiteral("unknown"),
                             .title = QStringLiteral("unknown")}));
    EXPECT_EQ(hydrated.resolvedBadges[2].title,
              QStringLiteral("missing-title"));
    EXPECT_EQ(hydrated.resolvedBadges[3].title, QStringLiteral("empty-title"));
    EXPECT_EQ(hydrated.resolvedBadges[4].title, QStringLiteral("wrong-title"));
    EXPECT_EQ(hydrated.resolvedBadges[5],
              (ResolvedBadge{.id = QStringLiteral("hostile"),
                             .title = hostileTitle}));
    ASSERT_TRUE(hydrated.roleIds);
    EXPECT_EQ(*hydrated.roleIds, (QStringList{QStringLiteral("broadcaster"),
                                              QStringLiteral("moderator")}));
}

TEST(RumbleBadge, MessageMembershipPrecedesUserAndDeltasReplaceUserBadges)
{
    QJsonObject catalog;
    for (const auto &id : {QStringLiteral("user"), QStringLiteral("message"),
                           QStringLiteral("delta")})
    {
        catalog.insert(id,
                       QJsonObject{{QStringLiteral("title"), id + " title"}});
    }

    auto initialMessage = messageObject(QStringLiteral("m1"));
    initialMessage.insert(QStringLiteral("badges"),
                          QJsonArray{QStringLiteral("message")});
    EventParser parser;
    const auto initialized = parser.parse(
        eventJson(QStringLiteral("init"),
                  QJsonObject{
                      {QStringLiteral("users"),
                       QJsonObject{
                           {QStringLiteral("u"),
                            QJsonObject{
                                {QStringLiteral("badges"),
                                 QJsonArray{QStringLiteral("user")}},
                                {QStringLiteral("roles"),
                                 QJsonArray{QStringLiteral("moderator")}},
                            }},
                       }},
                      {QStringLiteral("config"),
                       QJsonObject{{QStringLiteral("badges"), catalog}}},
                      {QStringLiteral("messages"), QJsonArray{initialMessage}},
                  }));
    ASSERT_TRUE(initialized.event);

    EventState state;
    auto first = state.process(*initialized.event, QStringLiteral("stream-a"));
    ASSERT_EQ(first.messages.size(), 1U);
    ASSERT_EQ(first.messages[0].resolvedBadges.size(), 1U);
    EXPECT_EQ(first.messages[0].resolvedBadges[0].id,
              QStringLiteral("message"));

    auto fromUser = messageObject(QStringLiteral("m2"));
    auto explicitMessage = messageObject(QStringLiteral("m3"));
    explicitMessage.insert(QStringLiteral("badges"),
                           QJsonArray{QStringLiteral("message")});
    auto explicitlyEmpty = messageObject(QStringLiteral("m4"));
    explicitlyEmpty.insert(QStringLiteral("badges"), QJsonArray{});
    const auto delta = parser.parse(
        eventJson(QStringLiteral("messages"),
                  QJsonObject{
                      {QStringLiteral("users"),
                       QJsonObject{
                           {QStringLiteral("u"),
                            QJsonObject{
                                {QStringLiteral("badges"),
                                 QJsonArray{QStringLiteral("delta")}},
                            }},
                       }},
                      {QStringLiteral("messages"),
                       QJsonArray{fromUser, explicitMessage, explicitlyEmpty}},
                  }));
    ASSERT_TRUE(delta.event);
    auto later = state.process(*delta.event, QStringLiteral("stream-a"));
    ASSERT_EQ(later.messages.size(), 3U);
    ASSERT_EQ(later.messages[0].resolvedBadges.size(), 1U);
    EXPECT_EQ(later.messages[0].resolvedBadges[0].id, QStringLiteral("delta"));
    ASSERT_EQ(later.messages[1].resolvedBadges.size(), 1U);
    EXPECT_EQ(later.messages[1].resolvedBadges[0].id,
              QStringLiteral("message"));
    EXPECT_TRUE(later.messages[2].resolvedBadges.empty());
    EXPECT_EQ(*later.messages[2].roleIds,
              QStringList{QStringLiteral("moderator")});
    EXPECT_EQ(*later.messages[0].roleIds,
              QStringList{QStringLiteral("moderator")});
}

TEST(RumbleBadge, ReinitReplacesCatalogAndSnapshotsSurviveInvalidation)
{
    const auto initFor = [](const QString &id, const QString &title,
                            bool includeDefinition) {
        QJsonObject catalog;
        if (includeDefinition)
        {
            catalog.insert(QStringLiteral("stable"),
                           QJsonObject{
                               {QStringLiteral("title"), title},
                               {QStringLiteral("icons"),
                                QJsonObject{
                                    {QStringLiteral("48"),
                                     QStringLiteral("/i/badges/stable_48.png")},
                                }},
                           });
        }
        auto message = messageObject(id);
        message.insert(QStringLiteral("badges"),
                       QJsonArray{QStringLiteral("stable")});
        return eventJson(QStringLiteral("init"),
                         QJsonObject{
                             {QStringLiteral("config"),
                              QJsonObject{{QStringLiteral("badges"), catalog}}},
                             {QStringLiteral("messages"), QJsonArray{message}},
                         });
    };

    EventParser parser;
    EventState state;
    const auto firstParsed = parser.parse(
        initFor(QStringLiteral("m1"), QStringLiteral("First"), true));
    ASSERT_TRUE(firstParsed.event);
    auto first = state.process(*firstParsed.event, QStringLiteral("stream-a"));
    ASSERT_EQ(first.messages.size(), 1U);
    const auto snapshot = first.messages.front();
    auto builtSnapshot = rumble::buildMessage(snapshot);
    ASSERT_EQ(snapshot.resolvedBadges.size(), 1U);
    EXPECT_EQ(snapshot.resolvedBadges[0].title, QStringLiteral("First"));

    const auto secondParsed = parser.parse(
        initFor(QStringLiteral("m2"), QStringLiteral("Second"), true));
    ASSERT_TRUE(secondParsed.event);
    auto second =
        state.process(*secondParsed.event, QStringLiteral("stream-a"));
    ASSERT_EQ(second.messages.size(), 1U);
    ASSERT_EQ(second.messages[0].resolvedBadges.size(), 1U);
    EXPECT_EQ(second.messages[0].resolvedBadges[0].title,
              QStringLiteral("Second"));
    EXPECT_EQ(snapshot.resolvedBadges[0].title, QStringLiteral("First"));
    const auto *snapshotBadge =
        dynamic_cast<const BadgeElement *>(builtSnapshot->elements[1].get());
    ASSERT_NE(snapshotBadge, nullptr);
    EXPECT_EQ(snapshotBadge->getTooltip(), QStringLiteral("First"));
    EXPECT_EQ(snapshotBadge->getEmote()->images.getImage1()->url().string,
              QStringLiteral("https://rumble.com/i/badges/stable_48.png"));

    const auto emptyParsed =
        parser.parse(initFor(QStringLiteral("m3"), QString{}, false));
    ASSERT_TRUE(emptyParsed.event);
    auto afterEmpty =
        state.process(*emptyParsed.event, QStringLiteral("stream-a"));
    ASSERT_EQ(afterEmpty.messages.size(), 1U);
    ASSERT_EQ(afterEmpty.messages[0].resolvedBadges.size(), 1U);
    EXPECT_EQ(afterEmpty.messages[0].resolvedBadges[0].title,
              QStringLiteral("stable"));

    const auto thirdParsed = parser.parse(
        initFor(QStringLiteral("m4"), QStringLiteral("Third"), true));
    ASSERT_TRUE(thirdParsed.event);
    auto third = state.process(*thirdParsed.event, QStringLiteral("stream-a"));
    ASSERT_EQ(third.messages.size(), 1U);
    EXPECT_EQ(third.messages[0].resolvedBadges[0].title,
              QStringLiteral("Third"));

    auto crossStreamMessage = messageObject(QStringLiteral("m5"));
    crossStreamMessage.insert(QStringLiteral("badges"),
                              QJsonArray{QStringLiteral("stable")});
    const auto crossStreamParsed =
        parser.parse(eventJson(QStringLiteral("messages"),
                               QJsonObject{{QStringLiteral("messages"),
                                            QJsonArray{crossStreamMessage}}}));
    ASSERT_TRUE(crossStreamParsed.event);
    auto crossStream =
        state.process(*crossStreamParsed.event, QStringLiteral("stream-b"));
    ASSERT_EQ(crossStream.messages.size(), 1U);
    ASSERT_EQ(crossStream.messages[0].resolvedBadges.size(), 1U);
    EXPECT_EQ(crossStream.messages[0].resolvedBadges[0].title,
              QStringLiteral("stable"));
}

TEST(RumbleBadge, RendersValidatedImagesAndEscapedTooltip)
{
    const auto hostileTitle = QStringLiteral("<b>Admin & \"owner\"</b>");
    MessageDto dto{
        .id = QStringLiteral("m"),
        .userId = QStringLiteral("u"),
        .channelId = QStringLiteral("c"),
        .text = QStringLiteral("body"),
        .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
        .timestamp = timestamp(),
        .loginName = QStringLiteral("login"),
        .displayName = QStringLiteral("Display"),
        .badgeIds = QStringList{QStringLiteral("moderator"), QString{},
                                QStringLiteral("unknown")},
        .roleIds = QStringList{QStringLiteral("broadcaster")},
        .source = QStringLiteral("public"),
        .resolvedBadges =
            {
                {.id = QStringLiteral("moderator"),
                 .title = hostileTitle,
                 .iconUrl48 = QStringLiteral(
                     "https://rumble.com/i/badges/moderator_48.png"),
                 .iconUrl96 = QStringLiteral(
                     "https://rumble.com/i/badges/moderator_96.png")},
                {.id = QStringLiteral("unknown"),
                 .title = QStringLiteral("unknown")},
            },
    };

    auto message = rumble::buildMessage(dto);
    ASSERT_TRUE(message);
    ASSERT_EQ(message->elements.size(), 4U);
    EXPECT_NE(dynamic_cast<TimestampElement *>(message->elements[0].get()),
              nullptr);
    const auto *first =
        dynamic_cast<const BadgeElement *>(message->elements[1].get());
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->getFlags(), MessageElementFlag::BadgeChannelAuthority);
    EXPECT_EQ(first->getTooltip(), hostileTitle.toHtmlEscaped());
    EXPECT_FALSE(first->getTooltip().contains(QStringLiteral("<")));
    ASSERT_TRUE(first->getEmote());
    EXPECT_EQ(first->getEmote()->id.string, QStringLiteral("moderator"));
    EXPECT_EQ(first->getEmote()->images.getImage1()->url().string,
              QStringLiteral("https://rumble.com/i/badges/moderator_48.png"));
    EXPECT_EQ(first->getEmote()->images.getImage2()->url().string,
              QStringLiteral("https://rumble.com/i/badges/moderator_96.png"));

    const auto *username = textElement(message, 2);
    ASSERT_NE(username, nullptr);
    EXPECT_EQ(username->words(), QStringList{QStringLiteral("Display:")});
    EXPECT_EQ(username->fontStyle(), FontStyle::ChatMediumBold);
    EXPECT_TRUE(
        std::ranges::none_of(message->elements, [](const auto &element) {
            const auto *text = dynamic_cast<const TextElement *>(element.get());
            return text != nullptr &&
                   text->words().contains(QStringLiteral("[unknown]"));
        }));
    ASSERT_TRUE(message->rumble);
    EXPECT_EQ(message->rumble->badgeIDs, *dto.badgeIds);
    EXPECT_EQ(message->rumble->roleIDs, *dto.roleIds);
}

TEST(RumbleBadge, UsesNormalVisibilityCategories)
{
    MessageDto dto{
        .id = QStringLiteral("m"),
        .userId = QStringLiteral("u"),
        .channelId = QStringLiteral("c"),
        .text = QStringLiteral("hello"),
        .timestamp = timestamp(),
        .resolvedBadges =
            {
                {.id = QStringLiteral("admin"),
                 .title = QStringLiteral("Admin"),
                 .iconUrl48 = QStringLiteral(
                     "https://rumble.com/i/badges/admin_48.png")},
                {.id = QStringLiteral("moderator"),
                 .title = QStringLiteral("Moderator"),
                 .iconUrl48 = QStringLiteral(
                     "https://rumble.com/i/badges/moderator_48.png")},
                {.id = QStringLiteral("recurring_subscription"),
                 .title = QStringLiteral("Subscriber"),
                 .iconUrl48 = QStringLiteral(
                     "https://rumble.com/i/badges/locals_48.png")},
                {.id = QStringLiteral("whale-blue"),
                 .title = QStringLiteral("Rumble Premium"),
                 .iconUrl48 = QStringLiteral(
                     "https://rumble.com/i/badges/whale_blue_48.png")},
            },
    };

    const auto message = rumble::buildMessage(dto);
    ASSERT_TRUE(message);
    ASSERT_EQ(message->elements.size(), 7U);
    EXPECT_EQ(message->elements[1]->getFlags(),
              MessageElementFlag::BadgeGlobalAuthority);
    EXPECT_EQ(message->elements[2]->getFlags(),
              MessageElementFlag::BadgeChannelAuthority);
    EXPECT_EQ(message->elements[3]->getFlags(),
              MessageElementFlag::BadgeSubscription);
    EXPECT_EQ(message->elements[4]->getFlags(),
              MessageElementFlag::BadgeVanity);
}

TEST(RumbleEmoteUnsupported, PreservesColonAndUnicodeTextAndIgnoresRanges)
{
    const std::vector<QString> inputs{
        QStringLiteral(":wave:"),
        QStringLiteral(":one::two: :wave::wave:"),
        QStringLiteral("e\u0301"),
        QStringLiteral("\U0001F600"),
        QStringLiteral("\U0001F469\u200D\U0001F4BB"),
        QStringLiteral("漢字"),
        QStringLiteral("עברית"),
    };

    std::size_t index = 0;
    for (const auto &input : inputs)
    {
        auto record = messageObject(QStringLiteral("m%1").arg(index++), input);
        record.insert(
            QStringLiteral("emotes"),
            QJsonArray{
                QJsonObject{{QStringLiteral("start"), -1},
                            {QStringLiteral("end"), 500},
                            {QStringLiteral("name"), QStringLiteral(":wave:")}},
                QJsonObject{{QStringLiteral("start"), 0},
                            {QStringLiteral("end"), 3},
                            {QStringLiteral("start_again"), 2}},
            });

        EventParser parser;
        const auto parsed = parser.parse(eventJson(
            QStringLiteral("messages"),
            QJsonObject{{QStringLiteral("messages"), QJsonArray{record}}}));
        ASSERT_TRUE(parsed.event);
        ASSERT_TRUE(std::holds_alternative<MessagesEvent>(*parsed.event));
        EXPECT_TRUE(parsed.diagnostics.empty());
        EventState state;
        auto output = state.process(*parsed.event, QStringLiteral("stream-a"));
        ASSERT_EQ(output.messages.size(), 1U);
        EXPECT_EQ(output.messages[0].text, input);

        auto message = rumble::buildMessage(output.messages[0]);
        ASSERT_TRUE(message);
        EXPECT_EQ(message->messageText, input);
        EXPECT_TRUE(
            std::ranges::none_of(message->elements, [](const auto &element) {
                return dynamic_cast<const EmoteElement *>(element.get()) !=
                       nullptr;
            }));
    }
}

TEST(RumbleAsset, BadgeRenderingAndCompletionStayBoundedAndOffline)
{
    CountingTransport transport;
    rumble::RumbleApi api(transport, [](std::function<void()> task) {
        task();
    });

    MessageDto dto{
        .id = QStringLiteral("m"),
        .userId = QStringLiteral("u"),
        .channelId = QStringLiteral("c"),
        .text = QStringLiteral(":clap:"),
        .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
        .timestamp = timestamp(),
        .badgeIds = QStringList{QStringLiteral("viewer")},
        .resolvedBadges = {{.id = QStringLiteral("viewer"),
                            .title = QStringLiteral("Viewer"),
                            .iconUrl48 = QStringLiteral(
                                "https://rumble.com/i/badges/viewer_48.png")}},
    };
    const auto message = rumble::buildMessage(dto);
    ASSERT_TRUE(message);
    EXPECT_TRUE(std::ranges::any_of(message->elements, [](const auto &element) {
        return dynamic_cast<const BadgeElement *>(element.get()) != nullptr;
    }));
    EXPECT_TRUE(
        std::ranges::none_of(message->elements, [](const auto &element) {
            return dynamic_cast<const ImageElement *>(element.get()) !=
                       nullptr ||
                   dynamic_cast<const EmoteElement *>(element.get()) != nullptr;
        }));

    AssetApplication app;
    Channel channel(QStringLiteral("rumble-assets"), Channel::Type::Rumble);
    EXPECT_FALSE(channel.messageSendContext().emoteCapabilitiesComplete);
    completion::EmoteSource source(
        &channel, std::make_unique<completion::ClassicEmoteStrategy>());
    source.update(QString{});
    ASSERT_FALSE(source.output().empty());
    for (const auto &candidate : source.output())
    {
        EXPECT_TRUE(candidate.isEmoji);
        EXPECT_EQ(candidate.identity.provider, QStringLiteral("emoji"));
        EXPECT_NE(candidate.identity.provider, QStringLiteral("rumble"));
    }

    std::vector<completion::StringCompletion> typed;
    source.addToStringCompletions(typed);
    ASSERT_FALSE(typed.empty());
    for (const auto &candidate : typed)
    {
        if (candidate.emote)
        {
            EXPECT_NE(candidate.emote->identity.provider,
                      QStringLiteral("rumble"));
        }
    }
    EXPECT_EQ(transport.starts, 0);
    (void)api;
}

}  // namespace
}  // namespace chatterino
