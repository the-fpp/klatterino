// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "lib/RumbleFixtureLoader.hpp"
#include "lib/RumbleFixtureTransport.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QString>

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chatterino {
namespace {

namespace fixture = ::chatterino::test;

QByteArray toByteArray(const std::string &bytes)
{
    return {bytes.data(), static_cast<int>(bytes.size())};
}

std::string joinChunks(const fixture::RumbleFixtureExchange &exchange)
{
    std::string joined;
    for (const auto &chunk : exchange.chunks)
    {
        joined += chunk.bytes;
    }
    return joined;
}

QJsonObject parseObject(const std::string &bytes)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(toByteArray(bytes), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        throw std::runtime_error("fixture JSON is not an object");
    }
    return document.object();
}

class SseCollector
{
public:
    void push(const std::string &chunk)
    {
        this->buffer_ += chunk;
        while (true)
        {
            const auto boundary = this->buffer_.find("\n\n");
            if (boundary == std::string::npos)
            {
                return;
            }

            const auto block = this->buffer_.substr(0, boundary);
            this->buffer_.erase(0, boundary + 2);
            this->parseBlock(block);
        }
    }

    [[nodiscard]] const std::vector<QJsonObject> &events() const
    {
        return this->events_;
    }

    [[nodiscard]] std::size_t malformedCount() const
    {
        return this->malformedCount_;
    }

    [[nodiscard]] const std::string &pendingBytes() const
    {
        return this->buffer_;
    }

private:
    void parseBlock(const std::string &block)
    {
        std::string data;
        std::size_t start = 0;
        while (start <= block.size())
        {
            const auto end = block.find('\n', start);
            auto line = block.substr(
                start, end == std::string::npos ? std::string::npos
                                                : end - start);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.starts_with("data:"))
            {
                auto value = line.substr(5);
                if (value.starts_with(' '))
                {
                    value.erase(0, 1);
                }
                if (!data.empty())
                {
                    data += '\n';
                }
                data += value;
            }

            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }

        if (data.empty())
        {
            return;
        }

        QJsonParseError error;
        const auto document =
            QJsonDocument::fromJson(toByteArray(data), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            ++this->malformedCount_;
            return;
        }
        this->events_.push_back(document.object());
    }

    std::string buffer_;
    std::vector<QJsonObject> events_;
    std::size_t malformedCount_ = 0;
};

struct ObservedResponse {
    std::vector<fixture::RumbleFixtureResponseHead> heads;
    std::vector<std::string> chunks;
    std::size_t completions = 0;
    std::vector<std::string> disconnects;
    std::int64_t finishedAtMs = 0;
};

ObservedResponse executeSingleScenario(const QString &name)
{
    auto script = fixture::loadRumbleFixtureScenario(name);
    const auto request = script.exchanges.front().expectedRequest;
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureTransport transport(scheduler, std::move(script));
    ObservedResponse observed;
    auto handle = transport.start(
        request,
        {
            .onHead = [&](const auto &head) {
                observed.heads.push_back(head);
            },
            .onBodyChunk = [&](const auto &chunk) {
                observed.chunks.push_back(chunk);
            },
            .onComplete = [&] {
                ++observed.completions;
            },
            .onDisconnect = [&](const auto &reason) {
                observed.disconnects.push_back(reason);
            },
        });
    scheduler.runUntilIdle();
    observed.finishedAtMs = scheduler.nowMs();
    EXPECT_FALSE(handle.active());
    EXPECT_EQ(transport.activeRequestCount(), 0U);
    EXPECT_EQ(transport.remainingExchangeCount(), 0U);
    return observed;
}

class FixtureSessionConsumer
{
public:
    explicit FixtureSessionConsumer(fixture::RumbleFixtureTransport &transport)
        : transport_(transport)
    {
    }

    void start()
    {
        this->requestChannelPage();
    }

    [[nodiscard]] const std::vector<int> &statuses() const
    {
        return this->statuses_;
    }

    [[nodiscard]] const SseCollector &sse() const
    {
        return this->sse_;
    }

    [[nodiscard]] bool disconnected() const
    {
        return this->disconnected_;
    }

    [[nodiscard]] const std::string &disconnectReason() const
    {
        return this->disconnectReason_;
    }

    [[nodiscard]] const QString &error() const
    {
        return this->error_;
    }

private:
    void requestChannelPage()
    {
        this->active_ = this->transport_.start(
            {
                .method = "GET",
                .target = "/c/fixture-channel/live/",
                .headers =
                    {
                        {"Accept", "text/html"},
                        {"User-Agent", "chatterino-rumble/1"},
                    },
            },
            {
                .onHead = [this](const auto &head) {
                    this->statuses_.push_back(head.status);
                },
                .onBodyChunk = [this](const auto &chunk) {
                    this->pageBytes_ += chunk;
                },
                .onComplete = [this] {
                    const QRegularExpression embedPattern(
                        R"(rumble\.com/embed/(v[0-9a-z]+)/)");
                    const auto match = embedPattern.match(
                        QString::fromStdString(this->pageBytes_));
                    if (!match.hasMatch())
                    {
                        this->error_ = "live page did not contain an embed ID";
                        return;
                    }
                    this->requestEmbed(match.captured(1));
                },
            });
    }

    void requestEmbed(const QString &embedId)
    {
        this->embedBytes_.clear();
        this->active_ = this->transport_.start(
            {
                .method = "GET",
                .target =
                    "/embedJS/u3/?request=video&ver=2&v=" +
                    embedId.toStdString(),
                .headers =
                    {
                        {"Accept", "application/json"},
                        {"User-Agent", "chatterino-rumble/1"},
                    },
            },
            {
                .onHead = [this](const auto &head) {
                    this->statuses_.push_back(head.status);
                },
                .onBodyChunk = [this](const auto &chunk) {
                    this->embedBytes_ += chunk;
                },
                .onComplete = [this] {
                    QJsonObject embed;
                    try
                    {
                        embed = parseObject(this->embedBytes_);
                    }
                    catch (const std::exception &)
                    {
                        this->error_ = "embed metadata was not valid JSON";
                        return;
                    }
                    const auto streamId = embed.value("vid").toInt();
                    if (streamId <= 0)
                    {
                        this->error_ = "embed metadata had no stream ID";
                        return;
                    }
                    this->requestSse(streamId);
                },
            });
    }

    void requestSse(int streamId)
    {
        this->active_ = this->transport_.start(
            {
                .method = "GET",
                .target = "/chat/api/chat/" + std::to_string(streamId) +
                          "/stream",
                .headers =
                    {
                        {"Accept", "text/event-stream"},
                        {"Cache-Control", "no-cache"},
                        {"Origin", "https://rumble.com"},
                        {"Referer", "https://rumble.com/"},
                        {"User-Agent", "chatterino-rumble/1"},
                    },
            },
            {
                .onHead = [this](const auto &head) {
                    this->statuses_.push_back(head.status);
                },
                .onBodyChunk = [this](const auto &chunk) {
                    this->sse_.push(chunk);
                },
                .onComplete = [this] {
                    this->error_ = "live fixture stream completed unexpectedly";
                },
                .onDisconnect = [this](const auto &reason) {
                    this->disconnected_ = true;
                    this->disconnectReason_ = reason;
                },
            });
    }

    fixture::RumbleFixtureTransport &transport_;
    fixture::RumbleFixtureRequestHandle active_;
    std::vector<int> statuses_;
    std::string pageBytes_;
    std::string embedBytes_;
    SseCollector sse_;
    bool disconnected_ = false;
    std::string disconnectReason_;
    QString error_;
};

TEST(RumbleFixtureScheduler, OrdersAndCancelsWithoutWallClockTime)
{
    fixture::ManualScheduler scheduler;
    std::vector<int> order;

    const auto cancelled = scheduler.scheduleAfter(5, [&] {
        order.push_back(99);
    });
    scheduler.scheduleAfter(0, [&] {
        order.push_back(1);
        scheduler.scheduleAfter(0, [&] {
            order.push_back(2);
        });
    });
    scheduler.scheduleAfter(5, [&] {
        order.push_back(3);
    });
    scheduler.cancel(cancelled);

    EXPECT_EQ(scheduler.runReady(), 2U);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(scheduler.advanceBy(4), 0U);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(scheduler.advanceBy(1), 1U);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(scheduler.nowMs(), 5);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);
    EXPECT_THROW(scheduler.advanceBy(-1), std::invalid_argument);
}

TEST(RumbleFixtureScheduler, CoarseAdvanceRunsChainedTasksAtTheirDeadlines)
{
    fixture::ManualScheduler scheduler;
    std::vector<std::int64_t> observedTimes;

    scheduler.scheduleAfter(5, [&] {
        observedTimes.push_back(scheduler.nowMs());
        scheduler.scheduleAfter(1, [&] {
            observedTimes.push_back(scheduler.nowMs());
        });
    });

    EXPECT_EQ(scheduler.advanceBy(10), 2U);
    EXPECT_EQ(observedTimes, (std::vector<std::int64_t>{5, 6}));
    EXPECT_EQ(scheduler.nowMs(), 10);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);
}

TEST(RumbleFixtureLoader, PreservesRawHttpAndSseBoundaries)
{
    const auto script = fixture::loadRumbleFixtureScenario("live-session");
    ASSERT_EQ(script.exchanges.size(), 3U);

    const auto &page = script.exchanges[0];
    EXPECT_EQ(page.response.status, 200);
    ASSERT_NE(fixture::findHeader(page.response.headers, "content-type"),
              nullptr);
    EXPECT_TRUE(joinChunks(page).starts_with("<!doctype html>"));

    const auto &embed = script.exchanges[1];
    EXPECT_EQ(parseObject(joinChunks(embed)).value("vid").toInt(), 1001);

    const auto &sse = script.exchanges[2];
    EXPECT_EQ(sse.response.status, 200);
    const auto *sseContentType =
        fixture::findHeader(sse.response.headers, "Content-Type");
    ASSERT_NE(sseContentType, nullptr);
    EXPECT_EQ(*sseContentType, "text/event-stream");
    ASSERT_EQ(sse.chunks.size(), 3U);
    EXPECT_EQ(sse.chunks[0].bytes, "data: ");
    EXPECT_EQ(sse.chunks[0].afterMs, 0);
    EXPECT_EQ(sse.chunks[1].afterMs, 7);
    EXPECT_EQ(sse.chunks[2].afterMs, 11);
    EXPECT_EQ(joinChunks(sse), fixture::readRumbleFixtureResource(
                                   ":/rumble/raw/sse-live.txt"));
    EXPECT_EQ(sse.terminal, fixture::RumbleFixtureTerminal::Disconnect);
    EXPECT_EQ(sse.terminalAfterMs, 3);
}

TEST(RumbleFixtureLoader, ProvenanceCoversEveryRawResource)
{
    const auto manifest = parseObject(
        fixture::readRumbleFixtureResource(":/rumble/scenarios.json"));
    std::set<QString> referenced;
    for (const auto scenarioValue : manifest.value("scenarios").toArray())
    {
        const auto scenario = scenarioValue.toObject();
        for (const auto exchangeValue :
             scenario.value("exchanges").toArray())
        {
            const auto response = exchangeValue.toObject()
                                      .value("response")
                                      .toObject();
            for (const auto chunkValue : response.value("chunks").toArray())
            {
                referenced.insert(
                    chunkValue.toObject().value("resource").toString());
            }
        }
    }

    const auto provenance = parseObject(
        fixture::readRumbleFixtureResource(":/rumble/provenance.json"));
    EXPECT_EQ(provenance.value("reviewed_on").toString(), "2026-07-17");
    std::set<QString> covered;
    for (const auto fixtureValue : provenance.value("fixtures").toArray())
    {
        const auto entry = fixtureValue.toObject();
        const auto resource = entry.value("resource").toString();
        SCOPED_TRACE(resource.toStdString());
        EXPECT_FALSE(resource.isEmpty());
        EXPECT_FALSE(entry.value("classification").toString().isEmpty());
        EXPECT_FALSE(entry.value("method").toString().isEmpty());
        EXPECT_FALSE(entry.value("statuses").toArray().isEmpty());
        EXPECT_FALSE(entry.value("content_type").toString().isEmpty());
        EXPECT_FALSE(entry.value("replacements").toArray().isEmpty());
        EXPECT_FALSE(entry.value("consumers").toArray().isEmpty());
        EXPECT_FALSE(fixture::readRumbleFixtureResource(resource).empty());
        EXPECT_TRUE(covered.insert(resource).second);
    }

    EXPECT_EQ(covered.size(), referenced.size());
    for (const auto &resource : referenced)
    {
        EXPECT_TRUE(covered.contains(resource));
    }
}

TEST(RumbleFixtureTransport, RejectsMismatchWithoutConsumingExchange)
{
    fixture::ManualScheduler scheduler;
    auto script = fixture::loadRumbleFixtureScenario("live-session");
    const auto firstRequest = script.exchanges.front().expectedRequest;
    fixture::RumbleFixtureTransport transport(scheduler, std::move(script));

    EXPECT_THROW(transport.start({.method = "GET", .target = "/wrong"}, {}),
                 std::logic_error);
    EXPECT_EQ(transport.remainingExchangeCount(), 3U);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);

    auto handle = transport.start(firstRequest, {});
    scheduler.runReady();
    EXPECT_FALSE(handle.active());
    EXPECT_EQ(transport.remainingExchangeCount(), 2U);
}

TEST(RumbleFixtureTransport, DrivesCompleteRawLiveSession)
{
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureTransport transport(
        scheduler, fixture::loadRumbleFixtureScenario("live-session"));
    FixtureSessionConsumer consumer(transport);

    consumer.start();
    scheduler.runReady();
    EXPECT_TRUE(consumer.error().isEmpty());
    EXPECT_FALSE(consumer.disconnected());
    EXPECT_TRUE(consumer.sse().events().empty());
    EXPECT_EQ(consumer.sse().pendingBytes(), "data: ");

    scheduler.advanceBy(7);
    EXPECT_TRUE(consumer.sse().events().empty());
    EXPECT_FALSE(consumer.sse().pendingBytes().empty());

    scheduler.advanceBy(11);
    ASSERT_EQ(consumer.sse().events().size(), 3U);
    EXPECT_EQ(consumer.sse().events()[0].value("type").toString(), "init");
    EXPECT_EQ(consumer.sse().events()[1].value("type").toString(),
              "messages");
    EXPECT_EQ(consumer.sse().events()[2].value("type").toString(),
              "delete_messages");
    const auto roleUpdate = consumer.sse().events()[1]
                                .value("data")
                                .toObject()
                                .value("users")
                                .toArray()[0]
                                .toObject()
                                .value("badges")
                                .toArray();
    ASSERT_EQ(roleUpdate.size(), 1);
    EXPECT_EQ(roleUpdate[0].toString(), "moderator");
    const auto deletedIds = consumer.sse().events()[2]
                                .value("data")
                                .toObject()
                                .value("message_ids")
                                .toArray();
    ASSERT_EQ(deletedIds.size(), 1);
    EXPECT_EQ(deletedIds[0].toString(), "m1");
    EXPECT_EQ(consumer.sse().malformedCount(), 0U);

    scheduler.advanceBy(3);
    EXPECT_TRUE(consumer.error().isEmpty());
    EXPECT_TRUE(consumer.disconnected());
    EXPECT_EQ(consumer.disconnectReason(), "fixture stream closed");
    EXPECT_EQ(consumer.statuses(), (std::vector<int>{200, 200, 200}));
    EXPECT_EQ(scheduler.nowMs(), 21);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);
    EXPECT_EQ(transport.activeRequestCount(), 0U);
    EXPECT_EQ(transport.remainingExchangeCount(), 0U);
}

TEST(RumbleFixtureTransport, ScriptsOfflineAndHttpFailuresIndependently)
{
    struct Case {
        const char *name;
        int status;
    };
    const std::vector<Case> cases{
        {"offline-sse-204", 204},
        {"rate-limit", 429},
        {"authentication-failure", 401},
        {"authorization-failure", 403},
        {"not-found", 404},
        {"server-error", 503},
    };

    for (const auto &testCase : cases)
    {
        SCOPED_TRACE(testCase.name);
        const auto observed = executeSingleScenario(testCase.name);
        ASSERT_EQ(observed.heads.size(), 1U);
        EXPECT_EQ(observed.heads[0].status, testCase.status);
        EXPECT_EQ(observed.completions, 1U);
        EXPECT_TRUE(observed.disconnects.empty());
    }

    const auto rateLimit = executeSingleScenario("rate-limit");
    const auto *rateLimitRetry =
        fixture::findHeader(rateLimit.heads[0].headers, "Retry-After");
    ASSERT_NE(rateLimitRetry, nullptr);
    EXPECT_EQ(*rateLimitRetry, "3");

    const auto serverError = executeSingleScenario("server-error");
    const auto *serverRetry =
        fixture::findHeader(serverError.heads[0].headers, "retry-after");
    ASSERT_NE(serverRetry, nullptr);
    EXPECT_EQ(*serverRetry, "5");

    const auto offlinePage = executeSingleScenario("offline-page");
    ASSERT_EQ(offlinePage.chunks.size(), 1U);
    EXPECT_NE(offlinePage.chunks[0].find("duration=\"3600\""),
              std::string::npos);
    EXPECT_NE(offlinePage.chunks[0].find("/embed/vstale/"),
              std::string::npos);

    const auto unavailable = executeSingleScenario("embed-unavailable");
    ASSERT_EQ(unavailable.chunks.size(), 1U);
    EXPECT_TRUE(parseObject(unavailable.chunks[0]).value("vid").isNull());
}

TEST(RumbleFixtureTransport, ReconnectSequenceIsFiniteAndDeterministic)
{
    auto script = fixture::loadRumbleFixtureScenario("reconnect");
    const auto firstRequest = script.exchanges[0].expectedRequest;
    const auto secondRequest = script.exchanges[1].expectedRequest;
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureTransport transport(scheduler, std::move(script));
    fixture::RumbleFixtureRequestHandle secondHandle;
    std::string firstBytes;
    std::string secondBytes;
    std::size_t disconnects = 0;
    std::size_t completions = 0;

    auto firstHandle = transport.start(
        firstRequest,
        {
            .onBodyChunk = [&](const auto &chunk) {
                firstBytes += chunk;
            },
            .onDisconnect = [&](const auto &) {
                ++disconnects;
                secondHandle = transport.start(
                    secondRequest,
                    {
                        .onBodyChunk = [&](const auto &chunk) {
                            secondBytes += chunk;
                        },
                        .onComplete = [&] {
                            ++completions;
                        },
                    });
            },
        });

    scheduler.runReady();
    EXPECT_FALSE(firstBytes.empty());
    EXPECT_TRUE(secondBytes.empty());
    EXPECT_TRUE(firstHandle.active());

    scheduler.advanceBy(5);
    EXPECT_EQ(disconnects, 1U);
    EXPECT_EQ(completions, 1U);
    EXPECT_FALSE(secondBytes.empty());
    EXPECT_FALSE(firstHandle.active());
    EXPECT_FALSE(secondHandle.active());
    EXPECT_EQ(transport.remainingExchangeCount(), 0U);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);

    SseCollector first;
    first.push(firstBytes);
    SseCollector second;
    second.push(secondBytes);
    ASSERT_EQ(first.events().size(), 1U);
    ASSERT_EQ(second.events().size(), 2U);
    const auto firstMessage = first.events()[0]
                                  .value("data")
                                  .toObject()
                                  .value("messages")
                                  .toArray()[0]
                                  .toObject();
    const auto repeatedMessage = second.events()[0]
                                     .value("data")
                                     .toObject()
                                     .value("messages")
                                     .toArray()[0]
                                     .toObject();
    EXPECT_EQ(firstMessage.value("id").toString(),
              repeatedMessage.value("id").toString());
}

TEST(RumbleFixtureTransport, PreservesAdversarialSseInputs)
{
    const auto malformed = executeSingleScenario("malformed-input");
    SseCollector malformedCollector;
    for (const auto &chunk : malformed.chunks)
    {
        malformedCollector.push(chunk);
    }
    EXPECT_TRUE(malformedCollector.events().empty());
    EXPECT_EQ(malformedCollector.malformedCount(), 1U);

    const auto duplicate = executeSingleScenario("duplicate-input");
    SseCollector duplicateCollector;
    for (const auto &chunk : duplicate.chunks)
    {
        duplicateCollector.push(chunk);
    }
    ASSERT_EQ(duplicateCollector.events().size(), 2U);
    const auto firstDuplicate = duplicateCollector.events()[0]
                                    .value("data")
                                    .toObject()
                                    .value("messages")
                                    .toArray()[0]
                                    .toObject();
    const auto secondDuplicate = duplicateCollector.events()[1]
                                     .value("data")
                                     .toObject()
                                     .value("messages")
                                     .toArray()[0]
                                     .toObject();
    EXPECT_EQ(firstDuplicate.value("id").toString(),
              secondDuplicate.value("id").toString());

    const auto outOfOrder = executeSingleScenario("out-of-order-input");
    SseCollector orderCollector;
    for (const auto &chunk : outOfOrder.chunks)
    {
        orderCollector.push(chunk);
    }
    ASSERT_EQ(orderCollector.events().size(), 2U);
    const auto later = orderCollector.events()[0]
                           .value("data")
                           .toObject()
                           .value("messages")
                           .toArray()[0]
                           .toObject();
    const auto earlier = orderCollector.events()[1]
                             .value("data")
                             .toObject()
                             .value("messages")
                             .toArray()[0]
                             .toObject();
    EXPECT_GT(QString::compare(later.value("time").toString(),
                              earlier.value("time").toString()),
              0);

    const auto unknown = executeSingleScenario("unknown-event");
    SseCollector unknownCollector;
    for (const auto &chunk : unknown.chunks)
    {
        unknownCollector.push(chunk);
    }
    ASSERT_EQ(unknownCollector.events().size(), 1U);
    EXPECT_EQ(unknownCollector.events()[0].value("type").toString(),
              "fixture_unknown");
}

TEST(RumbleFixtureTransport, DelayedDeliveryUsesOnlyVirtualTime)
{
    auto script = fixture::loadRumbleFixtureScenario("delayed-delivery");
    const auto request = script.exchanges[0].expectedRequest;
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureTransport transport(scheduler, std::move(script));
    std::size_t heads = 0;
    std::size_t chunks = 0;
    std::size_t completions = 0;

    auto handle = transport.start(
        request,
        {
            .onHead = [&](const auto &) {
                ++heads;
            },
            .onBodyChunk = [&](const auto &) {
                ++chunks;
            },
            .onComplete = [&] {
                ++completions;
            },
        });

    scheduler.runReady();
    EXPECT_EQ(heads, 1U);
    EXPECT_EQ(chunks, 0U);
    EXPECT_EQ(completions, 0U);
    scheduler.advanceBy(249);
    EXPECT_EQ(chunks, 0U);
    scheduler.advanceBy(1);
    EXPECT_EQ(chunks, 1U);
    EXPECT_EQ(completions, 0U);
    scheduler.advanceBy(10);
    EXPECT_EQ(completions, 1U);
    EXPECT_FALSE(handle.active());
    EXPECT_EQ(scheduler.nowMs(), 260);
}

TEST(RumbleFixtureTransport, CancellationReleasesCallbacksAndTimers)
{
    auto script = fixture::loadRumbleFixtureScenario("cancellation");
    const auto request = script.exchanges[0].expectedRequest;
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureTransport transport(scheduler, std::move(script));
    auto retainedByCallback = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = retainedByCallback;
    std::size_t callbacks = 0;

    auto handle = transport.start(
        request,
        {
            .onBodyChunk = [retainedByCallback, &callbacks](const auto &) {
                ++callbacks;
            },
            .onDisconnect = [&](const auto &) {
                ++callbacks;
            },
        });
    retainedByCallback.reset();
    ASSERT_FALSE(lifetime.expired());
    ASSERT_GT(scheduler.pendingTaskCount(), 0U);
    scheduler.runReady();

    handle.cancel();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(handle.active());
    EXPECT_EQ(transport.activeRequestCount(), 0U);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);
    scheduler.advanceBy(1000);
    EXPECT_EQ(callbacks, 0U);
}

TEST(RumbleFixtureTransport, CancelledGenerationCannotCompleteStale)
{
    auto script = fixture::loadRumbleFixtureScenario("stale-completion");
    const auto oldRequest = script.exchanges[0].expectedRequest;
    const auto newRequest = script.exchanges[1].expectedRequest;
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureTransport transport(scheduler, std::move(script));
    std::size_t oldHeads = 0;
    std::size_t oldChunks = 0;
    std::size_t oldCompletions = 0;
    std::size_t newCompletions = 0;

    auto oldHandle = transport.start(
        oldRequest,
        {
            .onHead = [&](const auto &) {
                ++oldHeads;
            },
            .onBodyChunk = [&](const auto &) {
                ++oldChunks;
            },
            .onComplete = [&] {
                ++oldCompletions;
            },
        });
    scheduler.runReady();
    ASSERT_EQ(oldHeads, 1U);
    ASSERT_EQ(oldChunks, 0U);
    oldHandle.cancel();

    auto newHandle = transport.start(
        newRequest,
        {
            .onComplete = [&] {
                ++newCompletions;
            },
        });
    scheduler.runReady();
    EXPECT_EQ(newCompletions, 1U);
    EXPECT_FALSE(newHandle.active());

    scheduler.advanceBy(100);
    EXPECT_EQ(oldChunks, 0U);
    EXPECT_EQ(oldCompletions, 0U);
    EXPECT_EQ(newCompletions, 1U);
    EXPECT_EQ(transport.activeRequestCount(), 0U);
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);
}

TEST(RumbleFixtureTransport, DestructionCancelsPendingConsumerLifetime)
{
    fixture::ManualScheduler scheduler;
    fixture::RumbleFixtureRequestHandle handle;
    auto retainedByCallback = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = retainedByCallback;
    std::size_t callbacks = 0;

    {
        auto script = fixture::loadRumbleFixtureScenario("cancellation");
        const auto request = script.exchanges[0].expectedRequest;
        auto transport = std::make_unique<fixture::RumbleFixtureTransport>(
            scheduler, std::move(script));
        handle = transport->start(
            request,
            {
                .onBodyChunk =
                    [retainedByCallback, &callbacks](const auto &) {
                        ++callbacks;
                    },
            });
        retainedByCallback.reset();
        ASSERT_FALSE(lifetime.expired());
    }

    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(handle.active());
    EXPECT_EQ(scheduler.pendingTaskCount(), 0U);
    scheduler.advanceBy(1000);
    EXPECT_EQ(callbacks, 0U);
}

}  // namespace
}  // namespace chatterino
