// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/RumbleFixtureLoader.hpp"
#include "messages/Link.hpp"
#include "messages/MessageElement.hpp"
#include "providers/rumble/RumbleEvent.hpp"
#include "providers/rumble/RumbleMessageBuilder.hpp"
#include "providers/rumble/RumbleSseParser.hpp"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace chatterino {
namespace {

using rumble::DeleteMessagesEvent;
using rumble::DeleteNonRantMessagesEvent;
using rumble::Event;
using rumble::EventParser;
using rumble::EventState;
using rumble::InitEvent;
using rumble::MessageDto;
using rumble::MessagesEvent;
using rumble::PinMessageEvent;
using rumble::SseParser;

void append(std::vector<QByteArray> &target, rumble::SseFeedResult result)
{
    std::ranges::move(result.records, std::back_inserter(target));
}

std::vector<QByteArray> feedOneByteAtATime(const QByteArray &input)
{
    SseParser parser;
    std::vector<QByteArray> records;
    for (const char byte : input)
    {
        append(records, parser.feed(QByteArrayView(&byte, 1)));
    }
    return records;
}

struct ScenarioParse {
    std::vector<Event> events;
    std::vector<rumble::Diagnostic> diagnostics;
};

ScenarioParse parseScenario(const QString &name)
{
    auto script = test::loadRumbleFixtureScenario(name);
    ScenarioParse parsed;
    EventParser eventParser;

    for (const auto &exchange : script.exchanges)
    {
        const auto *contentType =
            test::findHeader(exchange.response.headers, "Content-Type");
        if (contentType == nullptr ||
            !contentType->starts_with("text/event-stream"))
        {
            continue;
        }

        SseParser sseParser;
        for (const auto &chunk : exchange.chunks)
        {
            auto framed = sseParser.feed(
                QByteArrayView(chunk.bytes.data(),
                               static_cast<qsizetype>(chunk.bytes.size())));
            std::ranges::move(framed.diagnostics,
                              std::back_inserter(parsed.diagnostics));
            for (const auto &record : framed.records)
            {
                auto event = eventParser.parse(record);
                std::ranges::move(event.diagnostics,
                                  std::back_inserter(parsed.diagnostics));
                if (event.event)
                {
                    parsed.events.push_back(std::move(*event.event));
                }
            }
        }
    }
    return parsed;
}

QByteArray eventWithMessages(const QByteArray &messages)
{
    return QByteArrayLiteral(R"({"type":"messages","data":{"messages":[)") +
           messages + QByteArrayLiteral("]}}");
}

QByteArray validMessage(
    const QByteArray &id,
    const QByteArray &createdOn = QByteArrayLiteral("2026-01-01T00:00:00Z"))
{
    return QByteArrayLiteral(R"({"id":")") + id +
           QByteArrayLiteral(
               R"(","user_id":"u","channel_id":"c","text":"hello","created_on":")") +
           createdOn + QByteArrayLiteral(R"("})");
}

QString diagnosticText(const std::vector<rumble::Diagnostic> &diagnostics)
{
    QString output;
    for (const auto &diagnostic : diagnostics)
    {
        output += diagnostic.code + ' ' + diagnostic.path + '\n';
    }
    return output;
}

TEST(RumbleSseParser, FramesWholeBufferEverySplitAndOneByteChunks)
{
    const auto input =
        QStringLiteral("data: {\"value\":\"héllo\"}\n\n").toUtf8();
    const auto expected = QStringLiteral("{\"value\":\"héllo\"}").toUtf8();

    {
        SseParser parser;
        auto result = parser.feed(input);
        ASSERT_EQ(result.records.size(), 1U);
        EXPECT_EQ(result.records[0], expected);
        EXPECT_TRUE(result.diagnostics.empty());
    }

    for (qsizetype split = 0; split <= input.size(); ++split)
    {
        SseParser parser;
        std::vector<QByteArray> records;
        append(records, parser.feed(QByteArrayView(input.constData(), split)));
        append(records, parser.feed(QByteArrayView(input.constData() + split,
                                                   input.size() - split)));
        ASSERT_EQ(records.size(), 1U) << "split " << split;
        EXPECT_EQ(records[0], expected) << "split " << split;
    }

    const auto oneByte = feedOneByteAtATime(input);
    ASSERT_EQ(oneByte.size(), 1U);
    EXPECT_EQ(oneByte[0], expected);
}

TEST(RumbleSseParser, HandlesLineFormsCommentsAndIncompleteTerminalFrame)
{
    const QByteArray input = ": comment\r\n"
                             "event: ignored\r\n"
                             "data: first\r\n"
                             "data:second\r\n"
                             "\r\n"
                             "\r\n"
                             ": comment only\r\n"
                             "\r\n"
                             "data: incomplete";
    SseParser parser;
    auto result = parser.feed(input);

    ASSERT_EQ(result.records.size(), 1U);
    EXPECT_EQ(result.records[0], QByteArray("first\nsecond"));
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST(RumbleSseParser, EnforcesLimitOnceAndRecoversAtBoundary)
{
    QByteArray atLimit("data: ");
    atLimit.append(
        QByteArray(SseParser::MAX_EVENT_BYTES - atLimit.size(), 'x'));
    atLimit.append("\n\n");

    SseParser parser;
    auto accepted = parser.feed(atLimit);
    ASSERT_EQ(accepted.records.size(), 1U);
    EXPECT_TRUE(accepted.diagnostics.empty());

    QByteArray overLimit("data: ");
    overLimit.append(QByteArray(SseParser::MAX_EVENT_BYTES, 'x'));
    overLimit.append("\n\n");
    overLimit.append("data: recovered\n\n");

    SseParser recovering;
    auto recovered = recovering.feed(overLimit);
    ASSERT_EQ(recovered.diagnostics.size(), 1U);
    EXPECT_EQ(recovered.diagnostics[0].code, "event_too_large");
    ASSERT_EQ(recovered.records.size(), 1U);
    EXPECT_EQ(recovered.records[0], "recovered");
}

TEST(RumbleEventParser, ParsesAllTypedVariantsAndLosslessIdentity)
{
    EventParser parser;
    const QByteArray init = R"({
      "type":"init",
      "data":{
        "users":{"u":{"username":"login","display_name":"Display","color":"#123456","badges":["viewer"],"roles":["member"],"source":"public"}},
        "channels":{"c":{"title":"Channel"}},
        "config":{"badges":{"viewer":{"title":"Viewer"}},"message_length_max":200},
        "messages":[{"id":9007199254740993,"user_id":"u","channel_id":"c","text":"hello","created_on":"2026-01-01T01:02:03+02:30"}]
      }
    })";
    auto parsedInit = parser.parse(init);
    ASSERT_TRUE(parsedInit.event);
    ASSERT_TRUE(std::holds_alternative<InitEvent>(*parsedInit.event));
    const auto &typedInit = std::get<InitEvent>(*parsedInit.event);
    ASSERT_EQ(typedInit.messages.size(), 1U);
    EXPECT_EQ(typedInit.messages[0].id, "9007199254740993");
    EXPECT_EQ(typedInit.messages[0].createdOn, "2026-01-01T01:02:03+02:30");
    EXPECT_EQ(typedInit.messages[0].timestamp,
              QDateTime::fromString("2026-01-01T01:02:03+02:30", Qt::ISODate)
                  .toUTC());
    EXPECT_EQ(typedInit.messageLengthMax, 200);
    EXPECT_EQ(typedInit.users["u"].roleIds.value(), QStringList{"member"});
    EXPECT_EQ(typedInit.badges["viewer"].title, "Viewer");

    auto messages = parser.parse(eventWithMessages(validMessage("opaque-id")));
    ASSERT_TRUE(messages.event);
    EXPECT_TRUE(std::holds_alternative<MessagesEvent>(*messages.event));

    auto deleted = parser.parse(
        R"({"type":"delete_messages","data":{"message_ids":["opaque-id",9007199254740993]}})");
    ASSERT_TRUE(deleted.event);
    ASSERT_TRUE(std::holds_alternative<DeleteMessagesEvent>(*deleted.event));
    EXPECT_EQ(std::get<DeleteMessagesEvent>(*deleted.event).messageIds,
              (QStringList{"opaque-id", "9007199254740993"}));

    auto cleared = parser.parse(
        R"({"type":"delete_non_rant_messages","data":{"message_ids":["opaque-id"],"clear":true}})");
    ASSERT_TRUE(cleared.event);
    EXPECT_TRUE(
        std::holds_alternative<DeleteNonRantMessagesEvent>(*cleared.event));

    auto pinned = parser.parse(
        R"({"type":"pin_message","data":{"message":{"id":"p","user_id":"u","channel_id":"c","text":"pin","created_on":"2026-01-01T00:00:00Z"}}})");
    ASSERT_TRUE(pinned.event);
    EXPECT_TRUE(std::holds_alternative<PinMessageEvent>(*pinned.event));
}

TEST(RumbleEventParser, ParsesCurrentInlineClientContract)
{
    EventParser parser;
    const QByteArray init = R"({
      "type":"init",
      "data":{
        "users":[{"id":123,"username":"CurrentUser","color":"#123456","badges":["viewer"]}],
        "channels":[{"id":456,"username":"Current Channel"}],
        "config":{"badges":{"viewer":{"label":{"en":"Viewer"}}},"message_length_max":200},
        "messages":[
          {"id":9007199254740993,"user_id":123,"text":"no channel id","time":"2026-01-01T01:02:03+02:30","blocks":[]},
          {"id":"rant","user_id":"123","channel_id":456,"text":"paid","time":"2026-01-01T00:00:04Z","type":"rant","rant":{"price_cents":500},"blocks":[]}
        ]
      }
    })";

    auto parsed = parser.parse(init);
    ASSERT_TRUE(parsed.event);
    ASSERT_TRUE(std::holds_alternative<InitEvent>(*parsed.event));
    EXPECT_TRUE(parsed.diagnostics.empty())
        << diagnosticText(parsed.diagnostics).toStdString();

    const auto &typed = std::get<InitEvent>(*parsed.event);
    ASSERT_EQ(typed.users.size(), 1);
    EXPECT_EQ(typed.users["123"].loginName,
              std::optional<QString>{QStringLiteral("CurrentUser")});
    ASSERT_EQ(typed.channels.size(), 1);
    EXPECT_EQ(typed.channels["456"].name,
              std::optional<QString>{QStringLiteral("Current Channel")});
    EXPECT_EQ(typed.badges["viewer"].title, QStringLiteral("Viewer"));
    ASSERT_EQ(typed.messages.size(), 2U);
    EXPECT_EQ(typed.messages[0].id, QStringLiteral("9007199254740993"));
    EXPECT_EQ(typed.messages[0].userId, QStringLiteral("123"));
    EXPECT_TRUE(typed.messages[0].channelId.isEmpty());
    EXPECT_EQ(typed.messages[0].createdOn,
              QStringLiteral("2026-01-01T01:02:03+02:30"));
    EXPECT_FALSE(typed.messages[0].rant);
    EXPECT_EQ(typed.messages[1].channelId, QStringLiteral("456"));
    EXPECT_TRUE(typed.messages[1].rant);

    EventState state;
    const auto hydrated =
        state.process(*parsed.event, QStringLiteral("current-stream"));
    ASSERT_EQ(hydrated.messages.size(), 2U);
    EXPECT_EQ(hydrated.messages[0].displayName,
              std::optional<QString>{QStringLiteral("CurrentUser")});
    EXPECT_EQ(hydrated.messages[0].channelName,
              std::optional<QString>{QStringLiteral("CurrentUser")});
    EXPECT_EQ(hydrated.messages[1].channelName,
              std::optional<QString>{QStringLiteral("Current Channel")});
    ASSERT_EQ(hydrated.messages[0].resolvedBadges.size(), 1U);
    EXPECT_EQ(hydrated.messages[0].resolvedBadges[0].title,
              QStringLiteral("Viewer"));
}

TEST(RumbleEventParser, InvalidRecordsDoNotPoisonSiblingsOrDiagnostics)
{
    EventParser parser;
    const QByteArray input = R"({
      "type":"messages",
      "data":{"messages":[
        {"id":"","user_id":"secret-user","channel_id":"secret-channel","text":"TOP SECRET","created_on":"2026-01-01T00:00:00Z"},
        {"id":-1,"user_id":"secret-user","channel_id":"secret-channel","text":"TOP SECRET","created_on":"2026-01-01T00:00:00Z"},
        {"id":1.5,"user_id":"secret-user","channel_id":"secret-channel","text":"TOP SECRET","created_on":"2026-01-01T00:00:00Z"},
        {"id":"valid","user_id":"u","channel_id":"c","text":"safe","created_on":"2026-01-01T00:00:00Z"}
      ]}
    })";
    auto result = parser.parse(input);
    ASSERT_TRUE(result.event);
    const auto &messages = std::get<MessagesEvent>(*result.event).messages;
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0].id, "valid");
    ASSERT_GE(result.diagnostics.size(), 3U);

    const auto renderedDiagnostics = diagnosticText(result.diagnostics);
    EXPECT_FALSE(renderedDiagnostics.contains("TOP SECRET"));
    EXPECT_FALSE(renderedDiagnostics.contains("secret-user"));
    EXPECT_FALSE(renderedDiagnostics.contains("secret-channel"));
}

TEST(RumbleEventParser, DynamicMapKeysAreRedactedFromDiagnostics)
{
    EventParser parser;
    auto result = parser.parse(R"({
      "type":"init",
      "data":{
        "users":{"SECRET_USER":"not-an-object"},
        "channels":{"SECRET_CHANNEL":false},
        "config":{"badges":{
          "SECRET_BADGE_OBJECT":false,
          "SECRET_BADGE_TITLE":{"title":17}
        }},
        "messages":[]
      }
    })");
    ASSERT_TRUE(result.event);
    ASSERT_GE(result.diagnostics.size(), 4U);

    const auto renderedDiagnostics = diagnosticText(result.diagnostics);
    EXPECT_FALSE(renderedDiagnostics.contains("SECRET_USER"));
    EXPECT_FALSE(renderedDiagnostics.contains("SECRET_CHANNEL"));
    EXPECT_FALSE(renderedDiagnostics.contains("SECRET_BADGE_OBJECT"));
    EXPECT_FALSE(renderedDiagnostics.contains("SECRET_BADGE_TITLE"));
    EXPECT_TRUE(renderedDiagnostics.contains("data.users[*]"));
    EXPECT_TRUE(renderedDiagnostics.contains("data.channels[*]"));
    EXPECT_TRUE(renderedDiagnostics.contains("data.config.badges[*]"));
    EXPECT_TRUE(renderedDiagnostics.contains("data.config.badges[*].title"));
}

TEST(RumbleEventParser, RejectsEachMissingRequiredFieldAndBadTimestamp)
{
    const std::vector<QByteArray> records{
        R"({"user_id":"u","channel_id":"c","text":"x","created_on":"2026-01-01T00:00:00Z"})",
        R"({"id":"m","channel_id":"c","text":"x","created_on":"2026-01-01T00:00:00Z"})",
        R"({"id":"m","user_id":"u","channel_id":"c","created_on":"2026-01-01T00:00:00Z"})",
        R"({"id":"m","user_id":"u","channel_id":"c","text":"x"})",
        R"({"id":"m","user_id":"u","channel_id":"c","text":"x","created_on":"not-an-instant"})",
    };

    for (const auto &record : records)
    {
        EventParser parser;
        auto result = parser.parse(eventWithMessages(record));
        ASSERT_TRUE(result.event);
        EXPECT_TRUE(std::get<MessagesEvent>(*result.event).messages.empty());
        EXPECT_FALSE(result.diagnostics.empty());
    }
}

TEST(RumbleEventParser, RecoversAfterMalformedAndUnknownFrames)
{
    EventParser parser;
    auto malformed = parser.parse("{not-json}");
    EXPECT_FALSE(malformed.event);
    ASSERT_EQ(malformed.diagnostics.size(), 1U);
    EXPECT_EQ(malformed.diagnostics[0].code, "malformed_json");

    auto unknown = parser.parse(R"({"type":"future_type","data":{"value":1}})");
    EXPECT_FALSE(unknown.event);
    ASSERT_EQ(unknown.diagnostics.size(), 1U);
    EXPECT_EQ(unknown.diagnostics[0].code, "unknown_event_type");

    auto valid = parser.parse(eventWithMessages(validMessage("after")));
    ASSERT_TRUE(valid.event);
    ASSERT_EQ(std::get<MessagesEvent>(*valid.event).messages.size(), 1U);
    EXPECT_EQ(std::get<MessagesEvent>(*valid.event).messages[0].arrivalOrdinal,
              0U);
}

TEST(RumbleEventParser, ParsesStateOperationFixture)
{
    const auto parsed = parseScenario("state-events");
    ASSERT_EQ(parsed.events.size(), 2U);
    ASSERT_TRUE(
        std::holds_alternative<DeleteNonRantMessagesEvent>(parsed.events[0]));
    const auto &clear = std::get<DeleteNonRantMessagesEvent>(parsed.events[0]);
    EXPECT_TRUE(clear.clearNonRant);
    EXPECT_EQ(clear.messageIds, (QStringList{"m1", "9007199254740993"}));
    ASSERT_TRUE(std::holds_alternative<PinMessageEvent>(parsed.events[1]));
    EXPECT_EQ(std::get<PinMessageEvent>(parsed.events[1]).message.id, "m_pin");
}

TEST(RumbleEventOrdering, SortsBootstrapAndPreservesRealtimeArrival)
{
    EventParser parser;
    const QByteArray bootstrap = R"({
      "type":"init",
      "data":{"messages":[
        {"id":"late","user_id":"u","channel_id":"c","text":"late","created_on":"2026-01-01T00:00:02Z"},
        {"id":"tie-a","user_id":"u","channel_id":"c","text":"a","created_on":"2026-01-01T00:00:00Z"},
        {"id":"tie-b","user_id":"u","channel_id":"c","text":"b","created_on":"2026-01-01T00:00:00Z"},
        {"id":"middle","user_id":"u","channel_id":"c","text":"middle","created_on":"2026-01-01T00:00:01Z"}
      ]}
    })";
    auto init = parser.parse(bootstrap);
    ASSERT_TRUE(init.event);

    EventState state;
    auto ordered = state.process(*init.event, "stream-a");
    ASSERT_EQ(ordered.messages.size(), 4U);
    EXPECT_EQ(ordered.messages[0].id, "tie-a");
    EXPECT_EQ(ordered.messages[1].id, "tie-b");
    EXPECT_EQ(ordered.messages[2].id, "middle");
    EXPECT_EQ(ordered.messages[3].id, "late");

    auto later = parser.parse(eventWithMessages(
        validMessage("realtime-later", "2026-01-01T00:00:05Z")));
    auto earlier = parser.parse(eventWithMessages(
        validMessage("realtime-earlier", "2026-01-01T00:00:04Z")));
    ASSERT_TRUE(later.event);
    ASSERT_TRUE(earlier.event);
    auto first = state.process(*later.event, "stream-a");
    auto second = state.process(*earlier.event, "stream-a");
    ASSERT_EQ(first.messages.size(), 1U);
    ASSERT_EQ(second.messages.size(), 1U);
    EXPECT_EQ(first.messages[0].id, "realtime-later");
    EXPECT_EQ(second.messages[0].id, "realtime-earlier");
}

TEST(RumbleEventOrdering, DeduplicatesReconnectWithFifoEvictionAndReset)
{
    rumble::MessageIdDeduplicator deduplicator(2);
    deduplicator.setStreamIdentity("stream-a");
    EXPECT_TRUE(deduplicator.accept("a"));
    EXPECT_TRUE(deduplicator.accept("b"));
    EXPECT_FALSE(deduplicator.accept("a"));  // duplicate does not move
    EXPECT_TRUE(deduplicator.accept("c"));   // evicts a, not b
    EXPECT_TRUE(deduplicator.accept("a"));
    EXPECT_EQ(deduplicator.size(), 2U);

    deduplicator.setStreamIdentity("stream-a");
    EXPECT_FALSE(deduplicator.accept("a"));
    deduplicator.setStreamIdentity("stream-b");
    EXPECT_TRUE(deduplicator.accept("a"));
}

TEST(RumbleDedup, EventStateEvictsDeterministicallyAtItsBound)
{
    const auto timestamp =
        QDateTime::fromString("2026-01-01T00:00:00Z", Qt::ISODate).toUTC();
    auto message = [&](QString id) {
        return MessageDto{
            .id = std::move(id),
            .userId = QStringLiteral("u"),
            .channelId = QStringLiteral("c"),
            .text = QStringLiteral("text"),
            .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
            .timestamp = timestamp,
        };
    };

    EventState state(2);
    MessagesEvent first;
    first.messages = {message(QStringLiteral("a")),
                      message(QStringLiteral("b"))};
    EXPECT_EQ(
        state.process(Event{first}, QStringLiteral("stream")).messages.size(),
        2U);

    MessagesEvent duplicate;
    duplicate.messages = {message(QStringLiteral("a"))};
    EXPECT_TRUE(state.process(Event{duplicate}, QStringLiteral("stream"))
                    .messages.empty());

    MessagesEvent evict;
    evict.messages = {message(QStringLiteral("c")),
                      message(QStringLiteral("a"))};
    const auto replayed = state.process(Event{evict}, QStringLiteral("stream"));
    ASSERT_EQ(replayed.messages.size(), 2U);
    EXPECT_EQ(replayed.messages[0].id, QStringLiteral("c"));
    EXPECT_EQ(replayed.messages[1].id, QStringLiteral("a"));
}

TEST(RumbleDedup, BoundsAndResetsPresentationCatalogs)
{
    const auto timestamp =
        QDateTime::fromString("2026-01-01T00:00:00Z", Qt::ISODate).toUTC();
    InitEvent init;
    for (int index = 1; index <= 3; ++index)
    {
        const auto suffix = QString::number(index);
        const auto user = QStringLiteral("u") + suffix;
        const auto channel = QStringLiteral("c") + suffix;
        const auto badge = QStringLiteral("b") + suffix;
        init.users.insert(user,
                          {.id = user,
                           .displayName = QStringLiteral("User ") + suffix,
                           .badgeIds = QStringList{badge}});
        init.channels.insert(
            channel,
            {.id = channel, .name = QStringLiteral("Channel ") + suffix});
        init.badges.insert(
            badge, {.id = badge, .title = QStringLiteral("Badge ") + suffix});
        init.messages.push_back({
            .id = QStringLiteral("m") + suffix,
            .userId = user,
            .channelId = channel,
            .text = QStringLiteral("text"),
            .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
            .timestamp = timestamp,
            .badgeIds = QStringList{badge},
        });
    }

    EventState state(2);
    const auto output = state.process(Event{init}, QStringLiteral("stream-a"));
    ASSERT_EQ(output.messages.size(), 3U);
    // Sorted FIFO insertion deterministically evicts the first user/channel.
    EXPECT_EQ(output.messages[0].displayName, QStringLiteral("u1"));
    EXPECT_EQ(output.messages[0].channelName, QStringLiteral("c1"));
    EXPECT_EQ(output.messages[1].displayName, QStringLiteral("User 2"));
    EXPECT_EQ(output.messages[1].channelName, QStringLiteral("Channel 2"));
    EXPECT_EQ(output.messages[2].displayName, QStringLiteral("User 3"));
    EXPECT_EQ(output.messages[2].channelName, QStringLiteral("Channel 3"));
    ASSERT_EQ(output.messages[1].resolvedBadges.size(), 1U);
    EXPECT_EQ(output.messages[1].resolvedBadges[0].title,
              QStringLiteral("Badge 2"));
    ASSERT_EQ(output.messages[2].resolvedBadges.size(), 1U);
    EXPECT_EQ(output.messages[2].resolvedBadges[0].title, QStringLiteral("b3"));

    MessagesEvent afterIdentityChange;
    afterIdentityChange.messages.push_back({
        .id = QStringLiteral("fresh"),
        .userId = QStringLiteral("u3"),
        .channelId = QStringLiteral("c3"),
        .text = QStringLiteral("text"),
        .createdOn = QStringLiteral("2026-01-01T00:00:00Z"),
        .timestamp = timestamp,
        .badgeIds = QStringList{QStringLiteral("b2")},
    });
    const auto reset =
        state.process(Event{afterIdentityChange}, QStringLiteral("stream-b"));
    ASSERT_EQ(reset.messages.size(), 1U);
    EXPECT_EQ(reset.messages[0].displayName, QStringLiteral("u3"));
    EXPECT_EQ(reset.messages[0].channelName, QStringLiteral("c3"));
    EXPECT_EQ(reset.messages[0].resolvedBadges[0].title, QStringLiteral("b2"));
}

TEST(RumbleEventParser, RejectsNonNormalizedStringMessageIds)
{
    for (const auto &id : std::vector<QByteArray>{
             QByteArrayLiteral(" leading"),
             QByteArrayLiteral("trailing "),
             QByteArrayLiteral("control\\u0000id"),
             QByteArrayLiteral("line\\u000aid"),
         })
    {
        const std::string traceId{id.constData(),
                                  static_cast<std::size_t>(id.size())};
        SCOPED_TRACE(traceId);
        EventParser parser;
        const auto parsed = parser.parse(eventWithMessages(validMessage(id)));
        ASSERT_TRUE(parsed.event);
        const auto &messages = std::get<MessagesEvent>(*parsed.event).messages;
        EXPECT_TRUE(messages.empty());
        EXPECT_NE(diagnosticText(parsed.diagnostics)
                      .indexOf(QStringLiteral("invalid_message_id")),
                  -1);
    }
}

TEST(RumbleEventOrdering, HydratesDeltasAndUsesNeutralDefaults)
{
    EventParser parser;
    auto initialized = parser.parse(R"({
      "type":"init",
      "data":{
        "users":{"u":{"username":"Login","badges":["viewer"],"roles":["member"],"source":"public"}},
        "channels":{"c":{"title":"Fixture Channel"}},
        "messages":[{"id":"m1","user_id":"u","channel_id":"c","text":"hello","created_on":"2026-01-01T00:00:00Z"}]
      }
    })");
    ASSERT_TRUE(initialized.event);

    EventState state;
    auto output = state.process(*initialized.event, "stream-a");
    ASSERT_EQ(output.messages.size(), 1U);
    const auto &message = output.messages[0];
    EXPECT_EQ(message.loginName.value(), "Login");
    EXPECT_EQ(message.displayName.value(), "Login");
    EXPECT_EQ(message.channelName.value(), "Fixture Channel");
    EXPECT_EQ(message.badgeIds.value(), QStringList{"viewer"});
    EXPECT_EQ(message.roleIds.value(), QStringList{"member"});
    EXPECT_EQ(message.source.value(), "public");
    EXPECT_EQ(message.color.value(), QColor(153, 153, 153));

    auto missing = parser.parse(eventWithMessages(validMessage("m2")));
    ASSERT_TRUE(missing.event);
    auto other = state.process(*missing.event, "different-stream");
    ASSERT_EQ(other.messages.size(), 1U);
    EXPECT_EQ(other.messages[0].loginName.value(), "u");
    EXPECT_EQ(other.messages[0].channelName.value(), "c");
    EXPECT_TRUE(other.messages[0].badgeIds->empty());
}

TEST(RumbleEventOrdering, ReplaysAllRequiredFixtureScenarios)
{
    {
        const auto live = parseScenario("live-session");
        ASSERT_EQ(live.events.size(), 3U);
        EventState state;
        std::vector<QString> ids;
        std::size_t operations = 0;
        for (const auto &event : live.events)
        {
            auto result = state.process(event, "1001");
            for (const auto &message : result.messages)
            {
                ids.push_back(message.id);
            }
            operations += result.operations.size();
        }
        EXPECT_EQ(ids, (std::vector<QString>{"m1", "m2"}));
        EXPECT_EQ(operations, 1U);
    }

    {
        const auto reconnect = parseScenario("reconnect");
        EventState state;
        std::vector<QString> ids;
        for (const auto &event : reconnect.events)
        {
            auto result = state.process(event, "1001");
            for (const auto &message : result.messages)
            {
                ids.push_back(message.id);
            }
        }
        EXPECT_EQ(ids, (std::vector<QString>{"m1", "m2"}));
    }

    {
        const auto malformed = parseScenario("malformed-input");
        EXPECT_TRUE(malformed.events.empty());
        ASSERT_EQ(malformed.diagnostics.size(), 1U);
        EXPECT_EQ(malformed.diagnostics[0].code, "malformed_json");
    }

    {
        const auto duplicate = parseScenario("duplicate-input");
        EventState state;
        std::size_t delivered = 0;
        for (const auto &event : duplicate.events)
        {
            delivered += state.process(event, "1001").messages.size();
        }
        EXPECT_EQ(delivered, 1U);
    }

    {
        const auto outOfOrder = parseScenario("out-of-order-input");
        EventState state;
        std::vector<QString> ids;
        for (const auto &event : outOfOrder.events)
        {
            for (const auto &message : state.process(event, "1001").messages)
            {
                ids.push_back(message.id);
            }
        }
        EXPECT_EQ(ids, (std::vector<QString>{"m_later", "m_earlier"}));
    }

    {
        const auto unknown = parseScenario("unknown-event");
        EXPECT_TRUE(unknown.events.empty());
        ASSERT_EQ(unknown.diagnostics.size(), 1U);
        EXPECT_EQ(unknown.diagnostics[0].code, "unknown_event_type");
    }
}

TEST(RumbleMessageBuilder, BuildsStableIdentityTextElementsAndLinks)
{
    const auto timestamp =
        QDateTime::fromString("2026-01-01T00:00:00Z", Qt::ISODate).toUTC();
    MessageDto dto{
        .id = "9007199254740993",
        .userId = "u_fixture",
        .channelId = "c_fixture",
        .text = "plain https://example.com/path",
        .createdOn = "2026-01-01T00:00:00Z",
        .timestamp = timestamp,
        .arrivalOrdinal = 7,
        .loginName = "FixtureUser",
        .displayName = "Fixture User",
        .color = QColor("#123456"),
        .channelName = "Fixture Channel",
        .badgeIds = QStringList{"viewer"},
        .roleIds = QStringList{"member"},
        .source = "public",
    };

    auto message = rumble::buildMessage(dto);
    ASSERT_TRUE(message);
    EXPECT_EQ(message->id, dto.id);
    EXPECT_EQ(message->userID, dto.userId);
    EXPECT_EQ(message->loginName, "fixtureuser");
    EXPECT_EQ(message->displayName, "Fixture User");
    EXPECT_EQ(message->channelName, "Fixture Channel");
    EXPECT_EQ(message->usernameColor, QColor("#123456"));
    EXPECT_EQ(message->serverReceivedTime, timestamp);
    EXPECT_EQ(message->parseTime, timestamp.time());
    EXPECT_EQ(message->platform, MessagePlatform::Rumble);
    EXPECT_TRUE(message->flags.isEmpty());
    EXPECT_EQ(message->messageText, dto.text);
    EXPECT_EQ(message->searchText,
              "fixtureuser Fixture User: plain https://example.com/path");
    ASSERT_TRUE(message->rumble);
    EXPECT_EQ(message->rumble->channelID, "c_fixture");
    EXPECT_EQ(message->rumble->badgeIDs, QStringList{"viewer"});
    EXPECT_EQ(message->rumble->roleIDs, QStringList{"member"});
    EXPECT_EQ(message->rumble->source, "public");

    ASSERT_EQ(message->elements.size(), 4U);
    EXPECT_NE(dynamic_cast<TimestampElement *>(message->elements[0].get()),
              nullptr);

    const auto *username =
        dynamic_cast<TextElement *>(message->elements[1].get());
    ASSERT_NE(username, nullptr);
    EXPECT_EQ(username->words(), (QStringList{"Fixture", "User:"}));
    EXPECT_TRUE(username->getFlags().has(MessageElementFlag::RumbleUsername));
    EXPECT_TRUE(username->getFlags().has(MessageElementFlag::Username));
    EXPECT_FALSE(username->getFlags().has(MessageElementFlag::KickUsername));
    EXPECT_EQ(username->getLink().type, Link::UserInfo);
    EXPECT_EQ(username->getLink().value, "fixtureuser");

    const auto *plain = dynamic_cast<TextElement *>(message->elements[2].get());
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->words(), QStringList{"plain"});

    const auto *link = dynamic_cast<LinkElement *>(message->elements[3].get());
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->original(), QStringList{"https://example.com/path"});
    EXPECT_EQ(link->getLink().type, Link::Url);
    EXPECT_EQ(link->getLink().value, "https://example.com/path");

    auto clone = message->clone();
    EXPECT_EQ(clone->userID, dto.userId);
    EXPECT_EQ(clone->platform, MessagePlatform::Rumble);
    EXPECT_EQ(clone->rumble, message->rumble);
}

}  // namespace
}  // namespace chatterino
