// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleEmotes.hpp"

#include "messages/MessageElement.hpp"
#include "providers/rumble/RumbleEvent.hpp"
#include "providers/rumble/RumbleMessageBuilder.hpp"

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <ranges>

namespace chatterino {
namespace {

QJsonObject emote(QString name, int position, QString file,
                  bool subscribersOnly = false)
{
    return {
        {QStringLiteral("name"), std::move(name)},
        {QStringLiteral("position"), position},
        {QStringLiteral("file"), std::move(file)},
        {QStringLiteral("is_subs_only"), subscribersOnly},
    };
}

QByteArray catalog(QJsonArray global, QJsonArray channel = {})
{
    QJsonArray groups{
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("global-group")},
            {QStringLiteral("channel_id"), QJsonValue::Null},
            {QStringLiteral("emotes"), std::move(global)},
        },
    };
    if (!channel.isEmpty())
    {
        groups.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("channel-group")},
            {QStringLiteral("channel_id"), QStringLiteral("synthetic")},
            {QStringLiteral("emotes"), std::move(channel)},
        });
    }
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("data"),
                              QJsonObject{{QStringLiteral("items"), groups}}},
                         })
        .toJson(QJsonDocument::Compact);
}

rumble::EmoteCatalog acceptedCatalog()
{
    auto parsed = rumble::parseEmoteCatalog(catalog(
        {emote(QStringLiteral("r+wave"), 1,
               QStringLiteral("https://1a-1791.com/video/z12/global.png"))},
        {emote(QStringLiteral("ChannelWave"), 7,
               QStringLiteral("https://1a-1791.com/video/z12/channel.webp")),
         emote(QStringLiteral("Subscriber"), 8,
               QStringLiteral("https://1a-1791.com/video/z12/subscriber.gif"),
               true)}));
    EXPECT_TRUE(parsed.catalog);
    EXPECT_TRUE(parsed.diagnostics.empty());
    return parsed.catalog.value_or(rumble::EmoteCatalog{});
}

rumble::MessageDto messageDto(QString id, QString text)
{
    return {
        .id = std::move(id),
        .userId = QStringLiteral("synthetic-user"),
        .channelId = QStringLiteral("synthetic-channel"),
        .text = std::move(text),
        .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
        .timestamp = QDateTime::fromString(
                         QStringLiteral("2026-01-01T00:00:00Z"), Qt::ISODate)
                         .toUTC(),
    };
}

TEST(RumbleEmoteCatalog, AcceptsOnlyAuthoritativeObservedShapes)
{
    const auto parsed = rumble::parseEmoteCatalog(catalog(
        {emote(QStringLiteral("r+wave"), 1,
               QStringLiteral("https://1a-1791.com/video/z12/global.png"))},
        {emote(QStringLiteral("Plain7"), 2,
               QStringLiteral("https://1a-1791.com/video/z12/channel.webp"),
               true)}));
    ASSERT_TRUE(parsed.catalog);
    EXPECT_TRUE(parsed.diagnostics.empty());
    ASSERT_EQ(parsed.catalog->emotes.size(), 2U);
    EXPECT_EQ(parsed.catalog->emotes[0].id, QStringLiteral("global-group:1"));
    EXPECT_EQ(parsed.catalog->emotes[0].insertionText(),
              QStringLiteral(":r+wave:"));
    EXPECT_EQ(parsed.catalog->emotes[0].scope, rumble::EmoteScope::Global);
    EXPECT_EQ(parsed.catalog->emotes[1].id, QStringLiteral("channel-group:2"));
    EXPECT_EQ(parsed.catalog->emotes[1].scope, rumble::EmoteScope::Channel);
    EXPECT_TRUE(parsed.catalog->emotes[1].subscribersOnly);

    const auto rejected = rumble::parseEmoteCatalog(catalog(
        {
            emote(QStringLiteral("wave"), 1,
                  QStringLiteral("https://1a-1791.com/video/z12/plain.png")),
            emote(QStringLiteral("r+evil"), 2,
                  QStringLiteral("https://example.com/video/z12/evil.png")),
            emote(QStringLiteral("r+query"), 3,
                  QStringLiteral(
                      "https://1a-1791.com/video/z12/image.png?redirect=1")),
        },
        {
            emote(QStringLiteral("r+globalOnly"), 4,
                  QStringLiteral("https://1a-1791.com/video/z12/wrong.png")),
            emote(QStringLiteral("ChannelOK"), 5,
                  QStringLiteral("http://1a-1791.com/video/z12/http.png")),
        }));
    ASSERT_TRUE(rejected.catalog);
    EXPECT_TRUE(rejected.catalog->emotes.empty());
    EXPECT_EQ(rejected.diagnostics.size(), 5U);

    QByteArray oversized(rumble::MAX_EMOTE_CATALOG_BYTES + 1, 'x');
    const auto bounded = rumble::parseEmoteCatalog(oversized);
    EXPECT_FALSE(bounded.catalog);
    ASSERT_EQ(bounded.diagnostics.size(), 1U);
    EXPECT_EQ(bounded.diagnostics[0].code,
              QStringLiteral("emote_catalog_limit"));
}

TEST(RumbleEmoteOccurrence, UsesExactCatalogTokensAndUtf16Boundaries)
{
    const auto accepted = acceptedCatalog();
    const QString text =
        QStringLiteral(
            "e\u0301 \U0001F469\u200D\U0001F4BB :r+wave::ChannelWave:, ") +
        QStringLiteral(":unknown: :r+wave: \u05E2\u05D1\u05E8\u05D9\u05EA");
    const auto resolved = rumble::resolveEmoteOccurrences(text, accepted);
    ASSERT_EQ(resolved.size(), 3U);
    EXPECT_EQ(resolved[0].start, text.indexOf(QStringLiteral(":r+wave:")));
    EXPECT_EQ(resolved[0].length, QStringLiteral(":r+wave:").size());
    EXPECT_EQ(resolved[1].start, text.indexOf(QStringLiteral(":ChannelWave:")));
    EXPECT_EQ(resolved[2].start, text.lastIndexOf(QStringLiteral(":r+wave:")));
    EXPECT_EQ(resolved[0].definition.id, resolved[2].definition.id);

    EXPECT_TRUE(
        rumble::resolveEmoteOccurrences(
            QStringLiteral(":R+wave: :r+missing: ordinary:colon"), accepted)
            .empty());
    EXPECT_EQ(rumble::resolveEmoteOccurrences(
                  QStringLiteral(":r+wave::r+wave::r+wave:"), accepted, 2)
                  .size(),
              2U);
}

TEST(RumbleEmoteRendering,
     HydrationSnapshotsCatalogAndPreservesTextLinksAndUnknownTokens)
{
    rumble::EventState state;
    state.replaceEmoteCatalog(QStringLiteral("stream-one"), acceptedCatalog());
    rumble::MessagesEvent event;
    event.messages.push_back(messageDto(
        QStringLiteral("first"),
        QStringLiteral("before :r+wave::ChannelWave: https://example.com/ ") +
            QStringLiteral(":unknown: after")));
    auto processed = state.process(event, QStringLiteral("stream-one"));
    ASSERT_EQ(processed.messages.size(), 1U);
    ASSERT_EQ(processed.messages[0].resolvedEmotes.size(), 2U);

    const auto snapshot = processed.messages[0];
    state.replaceEmoteCatalog(QStringLiteral("stream-one"), {});
    EXPECT_EQ(snapshot.resolvedEmotes.size(), 2U);
    const auto built = rumble::buildMessage(snapshot);
    ASSERT_TRUE(built);
    EXPECT_EQ(built->messageText, snapshot.text);
    EXPECT_EQ(
        std::ranges::count_if(built->elements,
                              [](const auto &element) {
                                  return dynamic_cast<const EmoteElement *>(
                                             element.get()) != nullptr;
                              }),
        2);
    EXPECT_TRUE(std::ranges::any_of(built->elements, [](const auto &element) {
        return dynamic_cast<const LinkElement *>(element.get()) != nullptr;
    }));

    rumble::MessagesEvent later;
    later.messages.push_back(
        messageDto(QStringLiteral("later"), QStringLiteral(":r+wave:")));
    auto withoutCatalog = state.process(later, QStringLiteral("stream-one"));
    ASSERT_EQ(withoutCatalog.messages.size(), 1U);
    EXPECT_TRUE(withoutCatalog.messages[0].resolvedEmotes.empty());

    state.replaceEmoteCatalog(QStringLiteral("stream-one"), acceptedCatalog());
    rumble::MessagesEvent changedStream;
    changedStream.messages.push_back(
        messageDto(QStringLiteral("other"), QStringLiteral(":r+wave:")));
    auto invalidated =
        state.process(changedStream, QStringLiteral("stream-two"));
    ASSERT_EQ(invalidated.messages.size(), 1U);
    EXPECT_TRUE(invalidated.messages[0].resolvedEmotes.empty());
}

TEST(RumbleEmoteRendering, InvalidOccurrencesFallBackToExactLiteralText)
{
    auto dto = messageDto(QStringLiteral("invalid"),
                          QStringLiteral(":r+wave: literal"));
    dto.resolvedEmotes = {
        {.start = -1,
         .length = 500,
         .definition = acceptedCatalog().emotes.front()},
    };
    const auto built = rumble::buildMessage(dto);
    ASSERT_TRUE(built);
    EXPECT_EQ(built->messageText, dto.text);
    EXPECT_TRUE(std::ranges::none_of(built->elements, [](const auto &element) {
        return dynamic_cast<const EmoteElement *>(element.get()) != nullptr;
    }));
}

}  // namespace
}  // namespace chatterino
