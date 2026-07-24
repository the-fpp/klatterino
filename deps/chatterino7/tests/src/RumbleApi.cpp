// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleApi.hpp"

#include "lib/RumbleFixtureApiTransport.hpp"
#include "lib/RumbleFixtureLoader.hpp"
#include "lib/RumbleFixtureTransport.hpp"
#include "providers/rumble/RumbleQtTransport.hpp"
#include "Test.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace chatterino;
using namespace chatterino::rumble;
using namespace chatterino::test;

namespace {

class DeferredQueue
{
public:
    RumbleApi::Defer dispatcher()
    {
        return [this](std::function<void()> task) {
            this->tasks_.push_back(std::move(task));
        };
    }

    std::size_t runAll()
    {
        std::size_t count = 0;
        while (!this->tasks_.empty())
        {
            auto task = std::move(this->tasks_.front());
            this->tasks_.pop_front();
            task();
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::size_t size() const
    {
        return this->tasks_.size();
    }

private:
    std::deque<std::function<void()>> tasks_;
};

ResponseHead head(int status, QByteArray contentType)
{
    ResponseHead result{.status = status};
    if (!contentType.isEmpty())
    {
        result.headers.push_back(
            {QByteArrayLiteral("Content-Type"), std::move(contentType)});
    }
    return result;
}

struct ImmediateExchange {
    QUrl expectedUrl;
    ResponseHead response;
    QByteArray body;
    std::optional<TransportFailure> failure;
    bool duplicateTerminal = false;
    bool returnNullHandle = false;
    bool throwAfterCallbacks = false;
};

class ImmediateHandle final : public TransportHandle
{
public:
    explicit ImmediateHandle(std::shared_ptr<bool> active)
        : active_(std::move(active))
    {
    }

    ~ImmediateHandle() override
    {
        this->cancel();
    }

    void cancel() noexcept override
    {
        if (this->active_)
        {
            *this->active_ = false;
        }
    }

    [[nodiscard]] bool active() const noexcept override
    {
        return this->active_ && *this->active_;
    }

private:
    std::shared_ptr<bool> active_;
};

class ImmediateTransport final : public Transport
{
public:
    explicit ImmediateTransport(std::vector<ImmediateExchange> exchanges)
        : exchanges_(std::move(exchanges))
    {
    }

    std::unique_ptr<TransportHandle> start(
        TransportRequest request, TransportCallbacks callbacks) override
    {
        EXPECT_LT(this->next_, this->exchanges_.size());
        if (this->next_ >= this->exchanges_.size())
        {
            throw std::logic_error("unexpected immediate request");
        }

        auto exchange = this->exchanges_[this->next_++];
        if (!exchange.expectedUrl.isEmpty())
        {
            EXPECT_EQ(request.url, exchange.expectedUrl);
        }

        auto active = std::make_shared<bool>(true);
        if (!exchange.failure || exchange.response.status != 0)
        {
            if (callbacks.onHead)
            {
                callbacks.onHead(exchange.response);
            }
            if (!exchange.body.isEmpty() && callbacks.onBodyChunk)
            {
                callbacks.onBodyChunk(exchange.body);
            }
        }
        if (exchange.failure)
        {
            if (callbacks.onFailure)
            {
                callbacks.onFailure(*exchange.failure);
            }
        }
        else if (callbacks.onComplete)
        {
            callbacks.onComplete();
        }

        if (exchange.duplicateTerminal)
        {
            if (callbacks.onComplete)
            {
                callbacks.onComplete();
            }
            if (callbacks.onFailure)
            {
                callbacks.onFailure(TransportFailure::Network);
            }
        }
        if (exchange.throwAfterCallbacks)
        {
            throw std::runtime_error("synthetic start failure after callbacks");
        }
        *active = false;
        if (exchange.returnNullHandle)
        {
            return nullptr;
        }
        return std::make_unique<ImmediateHandle>(std::move(active));
    }

    [[nodiscard]] std::size_t requestCount() const
    {
        return this->next_;
    }

private:
    std::vector<ImmediateExchange> exchanges_;
    std::size_t next_ = 0;
};

class ThrowingTransport final : public Transport
{
public:
    std::unique_ptr<TransportHandle> start(TransportRequest,
                                           TransportCallbacks) override
    {
        throw std::runtime_error("exception-canary-secret");
    }
};

class ControlledTransport;

class ControlledHandle final : public TransportHandle
{
public:
    ControlledHandle(ControlledTransport &owner, std::shared_ptr<bool> active)
        : owner_(owner)
        , active_(std::move(active))
    {
    }

    ~ControlledHandle() override;

    void cancel() noexcept override;
    [[nodiscard]] bool active() const noexcept override
    {
        return this->active_ && *this->active_;
    }

private:
    ControlledTransport &owner_;
    std::shared_ptr<bool> active_;
};

class ControlledTransport final : public Transport
{
public:
    std::unique_ptr<TransportHandle> start(
        TransportRequest request, TransportCallbacks callbacks) override
    {
        this->request_ = std::move(request);
        this->active_ = std::make_shared<bool>(true);
        this->callbacks_ = std::move(callbacks);
        return std::make_unique<ControlledHandle>(*this, this->active_);
    }

    void cancelled() noexcept
    {
        ++this->cancelCount_;
        if (this->active_)
        {
            *this->active_ = false;
        }
    }

    // Deliberately simulates a non-conforming late reply. RumbleApi must still
    // ignore it after its owner/cancellation state has ended.
    void deliverLate()
    {
        if (this->callbacks_.onHead)
        {
            this->callbacks_.onHead(
                head(200, QByteArrayLiteral("application/json")));
        }
        if (this->callbacks_.onBodyChunk)
        {
            this->callbacks_.onBodyChunk(
                QByteArrayLiteral(R"({"vid":1001,"title":"late"})"));
        }
        if (this->callbacks_.onComplete)
        {
            this->callbacks_.onComplete();
        }
    }

    void deliverHead(ResponseHead response)
    {
        if (this->callbacks_.onHead)
        {
            this->callbacks_.onHead(response);
        }
    }

    void deliverBody(QByteArray body)
    {
        if (this->callbacks_.onBodyChunk)
        {
            this->callbacks_.onBodyChunk(body);
        }
    }

    void complete()
    {
        if (this->callbacks_.onComplete)
        {
            this->callbacks_.onComplete();
        }
    }

    void fail(TransportFailure failure)
    {
        if (this->callbacks_.onFailure)
        {
            this->callbacks_.onFailure(failure);
        }
    }

    [[nodiscard]] int cancelCount() const
    {
        return this->cancelCount_;
    }

    const TransportRequest &request() const
    {
        return this->request_;
    }

private:
    std::shared_ptr<bool> active_;
    TransportCallbacks callbacks_;
    TransportRequest request_;
    int cancelCount_ = 0;
};

ControlledHandle::~ControlledHandle()
{
    this->cancel();
}

void ControlledHandle::cancel() noexcept
{
    if (this->active_ && *this->active_)
    {
        this->owner_.cancelled();
    }
}

struct QtReplyScript {
    int status = 200;
    std::vector<Header> headers;
    QByteArray body;
    bool notifyReadyRead = true;
    std::optional<QUrl> redirect;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    bool finish = true;
    std::shared_ptr<std::atomic<int>> abortCount;
    std::shared_ptr<std::atomic<int>> destructionCount;
    QThread *replyThread = nullptr;
};

class FakeNetworkReply final : public QNetworkReply
{
public:
    FakeNetworkReply(QNetworkAccessManager::Operation operation,
                     const QNetworkRequest &request, QtReplyScript script,
                     QObject *parent)
        : QNetworkReply(parent)
        , script_(std::move(script))
    {
        this->setOperation(operation);
        this->setRequest(request);
        this->setUrl(request.url());
        this->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    ~FakeNetworkReply() override
    {
        if (this->script_.destructionCount)
        {
            this->script_.destructionCount->fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    void deliver()
    {
        if (this->isFinished())
        {
            return;
        }
        if (this->script_.status != 0)
        {
            this->setAttribute(QNetworkRequest::HttpStatusCodeAttribute,
                               this->script_.status);
        }
        for (const auto &header : this->script_.headers)
        {
            this->setRawHeader(header.name, header.value);
        }
        if (this->script_.redirect)
        {
            this->setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                               *this->script_.redirect);
        }
        Q_EMIT this->metaDataChanged();
        if (this->isFinished())
        {
            return;
        }

        this->pending_.append(this->script_.body);
        if (!this->pending_.isEmpty() && this->script_.notifyReadyRead)
        {
            Q_EMIT this->readyRead();
        }
        if (this->isFinished() || !this->script_.finish)
        {
            return;
        }
        if (this->script_.error != QNetworkReply::NoError)
        {
            this->setError(this->script_.error,
                           QStringLiteral("scripted transport failure"));
        }
        this->setFinished(true);
        Q_EMIT this->finished();
    }

    void deliverChunk(QByteArray bytes)
    {
        if (this->isFinished())
            return;
        this->pending_.append(bytes);
        Q_EMIT this->readyRead();
    }

    void finishNow()
    {
        if (this->isFinished())
            return;
        this->setFinished(true);
        Q_EMIT this->finished();
    }

    void abort() override
    {
        if (this->isFinished())
        {
            return;
        }
        if (this->script_.abortCount)
        {
            this->script_.abortCount->fetch_add(1, std::memory_order_relaxed);
        }
        this->setError(QNetworkReply::OperationCanceledError,
                       QStringLiteral("scripted abort"));
        this->setFinished(true);
        Q_EMIT this->finished();
    }

    [[nodiscard]] bool isSequential() const override
    {
        return true;
    }

    [[nodiscard]] qint64 bytesAvailable() const override
    {
        return this->pending_.size() + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (maxSize <= 0 || this->pending_.isEmpty())
        {
            return 0;
        }
        const auto count =
            std::min(maxSize, static_cast<qint64>(this->pending_.size()));
        std::memcpy(data, this->pending_.constData(),
                    static_cast<std::size_t>(count));
        this->pending_.remove(0, static_cast<int>(count));
        return count;
    }

private:
    QtReplyScript script_;
    QByteArray pending_;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit FakeNetworkAccessManager(std::vector<QtReplyScript> scripts)
    {
        for (auto &script : scripts)
        {
            this->scripts_.push_back(std::move(script));
        }
    }

    [[nodiscard]] const std::vector<QNetworkRequest> &requests() const
    {
        return this->requests_;
    }

    FakeNetworkReply *lastReply() const
    {
        return this->lastReply_;
    }

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *) override
    {
        this->requests_.push_back(request);
        if (this->scripts_.empty())
        {
            ADD_FAILURE() << "unexpected Qt network request";
            this->scripts_.push_back({
                .status = 0,
                .error = QNetworkReply::UnknownNetworkError,
            });
        }

        auto script = std::move(this->scripts_.front());
        this->scripts_.pop_front();
        auto *replyThread = script.replyThread;
        auto *reply =
            new FakeNetworkReply(operation, request, std::move(script),
                                 replyThread ? nullptr : this);
        this->lastReply_ = reply;
        if (replyThread)
        {
            reply->moveToThread(replyThread);
        }
        else
        {
            QTimer::singleShot(0, reply, [reply] {
                reply->deliver();
            });
        }
        return reply;
    }

private:
    std::deque<QtReplyScript> scripts_;
    std::vector<QNetworkRequest> requests_;
    QPointer<FakeNetworkReply> lastReply_;
};

struct QtCallbackLog {
    std::vector<QString> order;
    QByteArray body;
    std::optional<ResponseHead> responseHead;
    int heads = 0;
    int completions = 0;
    int failures = 0;
    std::optional<TransportFailure> failure;

    TransportCallbacks callbacks()
    {
        return {
            .onHead =
                [this](const ResponseHead &head) {
                    this->responseHead = head;
                    ++this->heads;
                    this->order.push_back(QStringLiteral("head"));
                },
            .onBodyChunk =
                [this](const QByteArray &chunk) {
                    this->body.append(chunk);
                    this->order.push_back(QStringLiteral("body"));
                },
            .onComplete =
                [this] {
                    ++this->completions;
                    this->order.push_back(QStringLiteral("complete"));
                },
            .onFailure =
                [this](TransportFailure value) {
                    ++this->failures;
                    this->failure = value;
                    this->order.push_back(QStringLiteral("failure"));
                },
        };
    }
};

void runQtEventLoop(int milliseconds = 10)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::processEvents();
}

QUrl embedUrl(const QString &id = QStringLiteral("vfixture"))
{
    QUrl url(QStringLiteral("https://rumble.com/embedJS/u3/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("request"), QStringLiteral("video"));
    query.addQueryItem(QStringLiteral("ver"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("v"), id);
    url.setQuery(query);
    return url;
}

ImmediateExchange validEmbed(QString id = QStringLiteral("1001"))
{
    return {
        .expectedUrl = embedUrl(),
        .response = head(200, QByteArrayLiteral("application/json")),
        .body = QStringLiteral(R"({"vid":%1,"title":"Fixture title"})")
                    .arg(id)
                    .toUtf8(),
    };
}

TransportRequest validSseRequest()
{
    return {
        .url = QUrl(QStringLiteral(
            "https://web7.rumble.com/chat/api/chat/1001/stream")),
        .headers =
            {
                {QByteArrayLiteral("Accept"),
                 QByteArrayLiteral("text/event-stream")},
                {QByteArrayLiteral("Cache-Control"),
                 QByteArrayLiteral("no-cache")},
                {QByteArrayLiteral("Origin"),
                 QByteArrayLiteral("https://rumble.com")},
                {QByteArrayLiteral("Referer"),
                 QByteArrayLiteral("https://rumble.com/")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::EventStream,
        .maxBodyBytes = 2 * 1024 * 1024,
        .timeoutMs = 20000,
        .maxRedirects = 3,
    };
}

TransportRequest validHtmlRequest(
    QString slug = QStringLiteral("fixture-channel"))
{
    return {
        .url = QUrl(QStringLiteral("https://rumble.com/c/%1/live/").arg(slug)),
        .headers =
            {
                {QByteArrayLiteral("Accept"), QByteArrayLiteral("text/html")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::Html,
    };
}

TransportRequest validEmbedRequest(QString id = QStringLiteral("vfixture"))
{
    return {
        .url = embedUrl(id),
        .headers =
            {
                {QByteArrayLiteral("Accept"),
                 QByteArrayLiteral("application/json")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::Json,
    };
}

TransportRequest validEmoteCatalogRequest()
{
    return {
        .url = QUrl(QStringLiteral(
            "https://rumble.com/service.php?name=emote.list&chat_id=1001")),
        .headers =
            {
                {QByteArrayLiteral("Accept"),
                 QByteArrayLiteral("application/json")},
                {QByteArrayLiteral("User-Agent"),
                 QByteArrayLiteral("chatterino-rumble/1")},
            },
        .expectedMediaType = ExpectedMediaType::Json,
        .maxBodyBytes = rumble::MAX_EMOTE_CATALOG_BYTES,
    };
}

}  // namespace

TEST(RumbleApiLocator, AcceptsOnlyCanonicalDocumentedForms)
{
    struct Case {
        QString input;
        LocatorKind kind;
        QString value;
        QString pagePath;
    };
    const std::vector<Case> cases = {
        {QStringLiteral("fixture-channel"),
         LocatorKind::Channel,
         QStringLiteral("fixture-channel"),
         {}},
        {QStringLiteral("0channel"),
         LocatorKind::Channel,
         QStringLiteral("0channel"),
         {}},
        {QStringLiteral("vfixture"),
         LocatorKind::Video,
         QStringLiteral("vfixture"),
         {}},
        {QStringLiteral("1001"),
         LocatorKind::Stream,
         QStringLiteral("1001"),
         {}},
        {QString(128, QChar(u'9')),
         LocatorKind::Stream,
         QString(128, QChar(u'9')),
         {}},
        {QStringLiteral("https://rumble.com/c/0/live/"),
         LocatorKind::Channel,
         QStringLiteral("0"),
         {}},
        {QStringLiteral("https://rumble.com/c/Fixture/live/?ignored=1#part"),
         LocatorKind::Channel,
         QStringLiteral("Fixture"),
         {}},
        {QStringLiteral("https://www.rumble.com/user/legacy/"),
         LocatorKind::Channel,
         QStringLiteral("legacy"),
         {}},
        {QStringLiteral("https://rumble.com/embed/vfixture/"),
         LocatorKind::Video,
         QStringLiteral("vfixture"),
         {}},
        {QStringLiteral("https://rumble.com/chat/popup/1001"),
         LocatorKind::Stream,
         QStringLiteral("1001"),
         {}},
        {QStringLiteral("https://rumble.com/vfixture-synthetic-title.html"),
         LocatorKind::VideoPage, QStringLiteral("vfixture"),
         QStringLiteral("/vfixture-synthetic-title.html")},
    };

    for (const auto &test : cases)
    {
        SCOPED_TRACE(test.input);
        const auto locator = RumbleApi::normalizeLocator(test.input);
        ASSERT_TRUE(locator);
        EXPECT_EQ(locator->kind, test.kind);
        EXPECT_EQ(locator->value, test.value);
        EXPECT_EQ(locator->pagePath, test.pagePath);
    }
}

TEST(RumbleApiLocator, RejectsAmbiguousHostsPathsAndEncodings)
{
    const std::vector<QString> rejected = {
        QString(),
        QStringLiteral("0"),
        QStringLiteral("00"),
        QStringLiteral("0001"),
        QString(80, QChar(u'0')),
        QString(129, QChar(u'9')),
        QStringLiteral("-1"),
        QStringLiteral("bad slug"),
        QStringLiteral("https://example.invalid/c/channel/live/"),
        QStringLiteral("http://rumble.com/c/channel/live/"),
        QStringLiteral("https://user@rumble.com/c/channel/live/"),
        QStringLiteral("https://@rumble.com/c/channel/live/"),
        QStringLiteral("https://rumble.com:443/c/channel/live/"),
        QStringLiteral("https://rumble.com:/c/channel/live/"),
        QStringLiteral("https://www.rumble.com:/user/channel/"),
        QStringLiteral("https://rumble.com./c/channel/live/"),
        QStringLiteral("https://rumble%2Ecom/c/channel/live/"),
        QStringLiteral("https://rumble.com%3A/c/channel/live/"),
        QStringLiteral("https://rumble.com/c/a%2Fb/live/"),
        QStringLiteral("https://rumble.com/c//channel/live/"),
        QStringLiteral("https://rumble.com/c/a%5Cb/live/"),
        QStringLiteral("https://rumble.com/c/%00/live/"),
        QStringLiteral("https://rumble.com/c/%2e%2e/live/"),
        QStringLiteral("https://rumble.com/c/%ZZ/live/"),
        QStringLiteral("https://rumble.com/service.php?name=user.login"),
        QStringLiteral("https://rumble.com/chat/popup/not-a-number"),
        QStringLiteral("https://rumble.com/embed/VUPPER/"),
        QStringLiteral("https://rumble.com/vbad"),
    };

    for (const auto &input : rejected)
    {
        SCOPED_TRACE(input);
        EXPECT_FALSE(RumbleApi::normalizeLocator(input));
    }
}

TEST(RumbleApiRetryAfter, ParsesDeltaAndStrictImfFixdate)
{
    const QDateTime now(QDate(2026, 7, 15), QTime(1, 0, 0), Qt::UTC);
    ASSERT_EQ(RumbleApi::parseRetryAfter("3", now), std::chrono::seconds(3));
    ASSERT_EQ(RumbleApi::parseRetryAfter("Wed, 15 Jul 2026 01:00:10 GMT", now),
              std::chrono::seconds(10));
    ASSERT_EQ(RumbleApi::parseRetryAfter("Wed, 15 Jul 2026 00:59:59 GMT", now),
              std::chrono::seconds(0));

    EXPECT_FALSE(
        RumbleApi::parseRetryAfter("Tue, 15 Jul 2026 01:00:10 GMT", now));
    EXPECT_FALSE(
        RumbleApi::parseRetryAfter("Mon, 30 Feb 2026 01:00:10 GMT", now));
    EXPECT_FALSE(
        RumbleApi::parseRetryAfter("Wed, 15 Jul 2026 25:00:10 GMT", now));
    EXPECT_FALSE(RumbleApi::parseRetryAfter("-1", now));
    EXPECT_FALSE(RumbleApi::parseRetryAfter("99999999999", now));
    EXPECT_FALSE(RumbleApi::parseRetryAfter("3\r\nX-Canary: secret", now));
}

TEST(RumbleResolverFixture, ResolvesAndBootstrapsCompleteRawScenario)
{
    ManualScheduler scheduler;
    RumbleFixtureTransport fixture(
        scheduler, loadRumbleFixtureScenario(QStringLiteral("live-session")));
    RumbleFixtureApiTransport transport(fixture);
    RumbleApi api(
        transport,
        [&](std::function<void()> callback) {
            scheduler.scheduleAfter(0, std::move(callback));
        },
        [] {
            return QDateTime(QDate(2026, 7, 15), QTime(1, 0), Qt::UTC);
        });

    std::optional<ResolveResult> resolved;
    auto resolution = api.resolve(QStringLiteral("fixture-channel"),
                                  [&](ResolveResult result) {
                                      resolved = std::move(result);
                                  });
    EXPECT_FALSE(resolved);
    scheduler.runUntilIdle();

    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->outcome, Outcome::ResolvedLive);
    ASSERT_TRUE(resolved->metadata);
    EXPECT_EQ(resolved->metadata->channelIdentity,
              QStringLiteral("fixture-channel"));
    EXPECT_TRUE(resolved->metadata->channelTitle.isEmpty());
    EXPECT_EQ(resolved->metadata->embedId, QStringLiteral("vfixture"));
    EXPECT_EQ(resolved->metadata->streamId, QStringLiteral("1001"));
    EXPECT_EQ(resolved->metadata->videoTitle, QStringLiteral("Fixture Stream"));
    EXPECT_EQ(fixture.remainingExchangeCount(), 1U);

    std::optional<BootstrapResult> bootstrapped;
    auto bootstrap = api.bootstrap(resolved->metadata->streamId,
                                   [&](BootstrapResult result) {
                                       bootstrapped = std::move(result);
                                   });
    scheduler.runUntilIdle();

    ASSERT_TRUE(bootstrapped);
    EXPECT_EQ(bootstrapped->outcome, Outcome::ResolvedLive);
    ASSERT_EQ(bootstrapped->events.size(), 3U);
    EXPECT_TRUE(
        std::holds_alternative<InitEvent>(bootstrapped->events.front()));
    EXPECT_EQ(fixture.remainingExchangeCount(), 0U);
}

TEST(RumbleResolverLimits, AcceptsProductionSizedResponseHeaders)
{
    auto page = head(200, QByteArrayLiteral("text/html"));
    page.headers.push_back({QByteArrayLiteral("Content-Security-Policy"),
                            QByteArray(20 * 1024, 'x')});
    ImmediateTransport transport({
        {
            .response = std::move(page),
            .body = QByteArrayLiteral(
                "<html><title>Live channel</title>"
                "<div class=\"videostream\" duration=\"0\">"
                "<iframe src=\"https://rumble.com/embed/vfixture/\">"
                "</iframe></div></html>"),
        },
        validEmbed(),
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<ResolveResult> result;
    auto handle = api.resolve(QStringLiteral("fixture-channel"),
                              [&](ResolveResult value) {
                                  result = std::move(value);
                              });
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_TRUE(result->metadata);
    EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    EXPECT_EQ(transport.requestCount(), 2U);
}

TEST(RumbleResolverDirect, ResolvesDecimalWithoutNetworkAndEmbedOffline)
{
    {
        ImmediateTransport transport({});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("1001"), [&](ResolveResult value) {
                result = std::move(value);
            });
        EXPECT_FALSE(result);
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->streamId, QStringLiteral("1001"));
        EXPECT_EQ(transport.requestCount(), 0U);
    }

    {
        ImmediateTransport transport({
            {
                .expectedUrl = embedUrl(),
                .response = head(200, QByteArrayLiteral("application/json")),
                .body = QByteArrayLiteral(R"({"vid":null,"unavailable":true})"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
        EXPECT_TRUE(result->metadata->streamId.isEmpty());
    }
}

TEST(RumbleResolverParser, FirstVideoDurationIsAuthoritative)
{
    ImmediateTransport liveTransport({
        {
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = QByteArrayLiteral(
                "<html><title>Live channel</title>"
                "<div class=\"tile videostream\" duration=\"0\">"
                "<iframe src=\"https://rumble.com/embed/vfixture/\">"
                "</iframe></div></html>"),
        },
        validEmbed(),
    });
    DeferredQueue liveQueue;
    RumbleApi liveApi(liveTransport, liveQueue.dispatcher());
    std::optional<ResolveResult> liveResult;
    auto liveHandle =
        liveApi.resolve(QStringLiteral("live"), [&](ResolveResult value) {
            liveResult = std::move(value);
        });
    liveQueue.runAll();

    ASSERT_TRUE(liveResult);
    EXPECT_EQ(liveResult->outcome, Outcome::ResolvedLive);
    ASSERT_TRUE(liveResult->metadata);
    EXPECT_EQ(liveResult->metadata->embedId, QStringLiteral("vfixture"));
    EXPECT_EQ(liveTransport.requestCount(), 2U);

    {
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = QByteArrayLiteral(
                "<html><title>Offline channel</title>"
                "<div class=\"videostream tile\" duration=\"3600\">"
                "<iframe src=\"https://rumble.com/embed/vstale/\"></iframe>"
                "</div><div class=\"videostream\" duration=\"0\">"
                "<iframe src=\"https://rumble.com/embed/vdecoy/\"></iframe>"
                "</div></html>"),
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("offline"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();

        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
        EXPECT_EQ(transport.requestCount(), 1U);
    }

    const std::vector<QByteArray> malformed = {
        QByteArrayLiteral("<html><title>Missing duration</title>"
                          "<div class=\"videostream\"></div></html>"),
        QByteArrayLiteral(
            "<html><title>Conflicting duration</title>"
            "<div class=\"videostream\" duration=\"0\" duration=\"1\">"
            "<iframe src=\"https://rumble.com/embed/vbad/\"></iframe>"
            "</div></html>"),
        QByteArrayLiteral(
            "<html><title>Wrong duration attribute</title>"
            "<div class=\"videostream\" data-duration=\"0\">"
            "<iframe src=\"https://rumble.com/embed/vbad/\"></iframe>"
            "</div></html>"),
        QByteArrayLiteral(
            "<html><title>Invalid duration</title>"
            "<div class=\"videostream\" duration=\"00\">"
            "<iframe src=\"https://rumble.com/embed/vbad/\"></iframe>"
            "</div></html>"),
        QByteArrayLiteral(
            "<html><title>Live without target</title>"
            "<div class=\"videostream\" duration=\"0\"></div></html>"),
    };
    for (const auto &body : malformed)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = body,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("malformed"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();

        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
        EXPECT_EQ(transport.requestCount(), 1U);
    }
}

TEST(RumbleResolverTransport, AcceptsCurrentDecodedVideoPageSize)
{
    constexpr qsizetype OBSERVED_PAGE_BYTES = 1827560;
    const QByteArray prefix = QByteArrayLiteral(
        "<!doctype html><html><head><title>Current large page</title>"
        "</head><body><script>");
    const QByteArray suffix = QByteArrayLiteral(
        "</script><iframe src=\"https://rumble.com/embed/vfixture/\">"
        "</iframe></body></html>");
    ASSERT_LT(prefix.size() + suffix.size(), OBSERVED_PAGE_BYTES);
    QByteArray page = prefix;
    page.append(
        QByteArray(OBSERVED_PAGE_BYTES - prefix.size() - suffix.size(), 'x'));
    page.append(suffix);
    ASSERT_EQ(page.size(), OBSERVED_PAGE_BYTES);

    FakeNetworkAccessManager manager({
        {
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/html; charset=utf-8")}},
            .body = std::move(page),
        },
        {
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("application/json")}},
            .body = QByteArrayLiteral(
                R"({"vid":1001,"title":"Current large page"})"),
        },
    });
    QObject owner;
    RumbleQtTransport transport(manager, &owner);
    RumbleApi api(transport, [&owner](std::function<void()> callback) {
        QTimer::singleShot(0, &owner, std::move(callback));
    });
    std::optional<ResolveResult> result;
    auto handle =
        api.resolve(QStringLiteral("https://rumble.com/vcurrent-large.html"),
                    [&](ResolveResult value) {
                        result = std::move(value);
                    });

    runQtEventLoop(100);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_TRUE(result->metadata);
    EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    EXPECT_EQ(result->metadata->streamId, QStringLiteral("1001"));
    ASSERT_EQ(manager.requests().size(), 2U);
}

TEST(RumbleResolverTransport, CompletesFromPagePrefixBeforeOversizedTail)
{
    const auto runCase = [](const QString &input, bool channelPage) {
        QByteArray page = QByteArrayLiteral(
            "<!doctype html><html><head><title>Prefix fixture</title>"
            "<script>");
        // Match the observed ordering: a large unrelated inline script comes
        // before the small structured embed record (about 631 KiB into the
        // supplied Chrome source).
        page.append(QByteArray(640 * 1024, 'x'));
        page.append(QByteArrayLiteral(
            "</script><script type=\"application/ld+json\">"
            "{\"embedUrl\":\"https://rumble.com/embed/vfixture/\"}"
            "</script></head><body>"));
        if (channelPage)
        {
            page.append(QByteArrayLiteral(
                "<div class=\"tile videostream\" duration=\"0\"></div>"));
        }
        ASSERT_LT(page.size(), 4 * 1024 * 1024);
        page.append(QByteArrayLiteral("<script>"));
        page.append(QByteArray(5 * 1024 * 1024, 'z'));
        page.append(QByteArrayLiteral("</script></body></html>"));
        ASSERT_GT(page.size(), 4 * 1024 * 1024);

        FakeNetworkAccessManager manager({
            {
                .status = 200,
                .headers = {{QByteArrayLiteral("Content-Type"),
                             QByteArrayLiteral("text/html; charset=utf-8")}},
                .body = std::move(page),
            },
            {
                .status = 200,
                .headers = {{QByteArrayLiteral("Content-Type"),
                             QByteArrayLiteral("application/json")}},
                .body = QByteArrayLiteral(
                    R"({"vid":1001,"title":"Prefix fixture"})"),
            },
        });
        QObject owner;
        RumbleQtTransport transport(manager, &owner);
        RumbleApi api(transport, [&owner](std::function<void()> callback) {
            QTimer::singleShot(0, &owner, std::move(callback));
        });
        std::optional<ResolveResult> result;
        auto handle = api.resolve(input, [&](ResolveResult value) {
            result = std::move(value);
        });

        runQtEventLoop(200);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
        EXPECT_EQ(result->metadata->streamId, QStringLiteral("1001"));
        ASSERT_EQ(manager.requests().size(), 2U);
    };

    runCase(QStringLiteral("https://rumble.com/vfixture-prefix.html"), false);
    runCase(QStringLiteral("prefix-channel"), true);
}

TEST(RumbleResolverFallback, UsesLegacyProfileOnlyAfterChannel404)
{
    const auto page404 =
        QUrl(QStringLiteral("https://rumble.com/c/legacy/live/"));
    const auto legacyPage =
        QUrl(QStringLiteral("https://rumble.com/user/legacy/live/"));
    ImmediateTransport transport({
        {
            .expectedUrl = page404,
            .response = head(404, QByteArrayLiteral("text/html")),
            .body = QByteArrayLiteral("<html><title>Missing</title></html>"),
        },
        {
            .expectedUrl = legacyPage,
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = QByteArrayLiteral(
                "<html><head><title>Legacy title</title></head>"
                "<body><iframe src=\"https://rumble.com/embed/vfixture/\">"
                "</iframe></body></html>"),
        },
        validEmbed(),
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());

    std::optional<ResolveResult> result;
    auto handle =
        api.resolve(QStringLiteral("legacy"), [&](ResolveResult value) {
            result = std::move(value);
        });
    EXPECT_FALSE(result);
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    EXPECT_EQ(transport.requestCount(), 3U);
    ASSERT_TRUE(result->metadata);
    EXPECT_TRUE(result->metadata->channelTitle.isEmpty());
}

TEST(RumbleResolverFallback, ReentrantCompletionIgnoresOldNullOrThrowingStart)
{
    for (const bool throwAfterCallbacks : {false, true})
    {
        SCOPED_TRACE(throwAfterCallbacks ? "throw" : "null");
        ImmediateExchange primary404{
            .expectedUrl =
                QUrl(QStringLiteral("https://rumble.com/c/legacy/live/")),
            .response = head(404, QByteArrayLiteral("text/plain")),
            .body = QByteArrayLiteral("not found"),
        };
        primary404.returnNullHandle = !throwAfterCallbacks;
        primary404.throwAfterCallbacks = throwAfterCallbacks;

        ImmediateTransport transport({
            std::move(primary404),
            {
                .expectedUrl = QUrl(
                    QStringLiteral("https://rumble.com/user/legacy/live/")),
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><title>Legacy</title>"
                    "<iframe src=\"https://rumble.com/embed/vfixture/\">"
                    "</iframe></html>"),
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("legacy"), [&](ResolveResult value) {
                result = std::move(value);
            });

        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        EXPECT_EQ(transport.requestCount(), 3U);
    }
}

TEST(RumbleResolverParser, IgnoresCommentsAndUntypedScripts)
{
    const auto page = QUrl(QStringLiteral("https://rumble.com/c/scoped/live/"));
    ImmediateTransport transport({
        {
            .expectedUrl = page,
            .response =
                head(200, QByteArrayLiteral("text/html; charset=utf-8")),
            .body = QByteArrayLiteral(
                "<html><head><title>Scoped page</title></head><body>"
                "<!-- <iframe src=\"https://rumble.com/embed/vcomment/\"> -->"
                "<script>const fake='<iframe "
                "src=\"https://rumble.com/embed/vscript/\">';"
                " const "
                "metadata={embedUrl:'https://rumble.com/embed/vscript2/'};"
                "</script>"
                "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
                "</body></html>"),
        },
        validEmbed(),
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());

    std::optional<ResolveResult> result;
    auto handle =
        api.resolve(QStringLiteral("scoped"), [&](ResolveResult value) {
            result = std::move(value);
        });
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_TRUE(result->metadata);
    EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
}

TEST(RumbleResolverParser, ToleratesBoundedUnrelatedHydrationMarkup)
{
    constexpr qsizetype candidateLimit = 4096;
    const QByteArray padding(candidateLimit + 1, 'x');
    const auto currentShape =
        QByteArrayLiteral(
            "<html><head><title>Current channel shape</title></head><body>"
            "<div data-hydration=\"") +
        padding +
        QByteArrayLiteral(
            " &lt;iframe src='https://rumble.com/embed/vdecoy/'&gt;\" "
            "data-layout=\"first\" data-layout=\"second\"></div>"
            "<script type=\"application/ld+json\">"
            "{\"video\":{\"embedUrl\":"
            "\"https://rumble.com/embed/vfixture/\"}}"
            "</script></body></html>");
    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = currentShape,
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("current"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();

        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    }

    const auto decoyOnly =
        QByteArrayLiteral("<html><title>Unrelated attribute stays inert</title>"
                          "<div data-hydration=\"") +
        padding +
        QByteArrayLiteral(" <iframe src='https://rumble.com/embed/vdecoy/'>\">"
                          "</div></html>");
    {
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = decoyOnly,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("offline"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();

        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><title>Quoted unrelated closer</title>"
                    "</div data=\">\">"
                    "<iframe src=\"https://rumble.com/embed/vfixture/\">"
                    "</iframe></html>"),
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle = api.resolve(QStringLiteral("quoted-closer"),
                                  [&](ResolveResult value) {
                                      result = std::move(value);
                                  });
        queue.runAll();

        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    }

    const std::vector<QByteArray> contractNearMisses = {
        QByteArrayLiteral(
            "<html><title>Contract prefix is not unrelated</title>"
            "<iframe@x></iframe>"
            "<iframe src=\"https://rumble.com/embed/vbad/\">"
            "</iframe></html>"),
        QByteArrayLiteral("<html><title>Oversized iframe</title>"
                          "<iframe data-padding=\"") +
            padding +
            QByteArrayLiteral("\" src=\"https://rumble.com/embed/vbad/\">"
                              "</iframe></html>"),
        QByteArrayLiteral(
            "<html><title>Duplicate iframe source</title>"
            "<iframe src=\"https://rumble.com/embed/vone/\" "
            "src=\"https://rumble.com/embed/vtwo/\"></iframe></html>"),
        QByteArrayLiteral("<html><title>Oversized typed script</title>"
                          "<script type=\"application/json\" data-padding=\"") +
            padding +
            QByteArrayLiteral(
                "\">{\"embedUrl\":"
                "\"https://rumble.com/embed/vbad/\"}</script></html>"),
        QByteArrayLiteral("<html><title>Oversized challenge form</title>"
                          "<form data-padding=\"") +
            padding +
            QByteArrayLiteral(
                "\" action=\"/cdn-cgi/challenge\"></form></html>"),
        QByteArrayLiteral("<html><title>Unterminated unrelated closer</title>"
                          "</div data=\"unterminated>"
                          "<iframe src='https://rumble.com/embed/vbad/'>"
                          "</iframe></html>"),
        QByteArrayLiteral(
            "<html><title>Nameless closing construct</title>"
            "</ data=\"><iframe "
            "src='https://rumble.com/embed/vbad/'></iframe>\"></html>"),
        QByteArrayLiteral("<html><template><div data-hydration=\"") + padding +
            QByteArrayLiteral(
                "\"></div></template><title>Strict inert scope</title>"
                "<iframe src=\"https://rumble.com/embed/vbad/\">"
                "</iframe></html>"),
        QByteArrayLiteral("<html><template></div junk></template>"
                          "<title>Strict inert close</title>"
                          "<iframe src=\"https://rumble.com/embed/vbad/\">"
                          "</iframe></html>"),
    };
    for (const auto &body : contractNearMisses)
    {
        SCOPED_TRACE(body.size());
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = body,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("rejected"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();

        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
        EXPECT_EQ(transport.requestCount(), 1U);
    }
}

TEST(RumbleResolverParser, ScopesTagsAttributesAndTypedJson)
{
    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><head>"
                    "<script>const fake='<title>Script title</title>';</script>"
                    "<title>Scoped attributes</title></head><body>"
                    "<div data-payload=\"<iframe "
                    "src='https://rumble.com/embed/vquoted/'>\"></div>"
                    "<iframe "
                    "data-src=\"https://rumble.com/embed/vdatasrc/\"></iframe>"
                    "<script data-type=\"application/json\">"
                    "{\"embedUrl\":\"https://rumble.com/embed/vdatatype/\"}"
                    "</script>"
                    "<form data-action=\"/cdn-cgi/challenge\"></form>"
                    "<template><iframe "
                    "src=\"https://rumble.com/embed/vtemplate/\">"
                    "</iframe></template>"
                    "<iframe "
                    "src=\"https://rumble.com/embed/vfixture/\"></iframe>"
                    "</body></html>"),
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("scoped"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body =
                    QByteArrayLiteral("<html><title>Typed metadata</title>"
                                      "<script type=\"application/ld+json\">"
                                      "{\"video\":{\"embedUrl\":\"https://"
                                      "rumble.com/embed/vfixture/\"}}"
                                      "</script></html>"),
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("typed"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    }
}

TEST(RumbleResolverParser, IgnoresAmbiguousTypedJsonAndRawTextDecoys)
{
    const std::vector<QByteArray> livePages = {
        QByteArrayLiteral(
            "<html><title>Duplicate JSON is inert</title>"
            "<script type=\"application/json\">"
            "{\"embedUrl\":\"https://rumble.com/embed/vone/\","
            "\"embed\\u0055rl\":\"https://rumble.com/embed/vtwo/\"}"
            "</script>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Xmp is inert</title>"
            "<xmp><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</iframe></xmp>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Self-closing xmp slash is ignored</title>"
            "<xmp/><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</iframe></xmp>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Self-closing template slash is ignored</title>"
            "<template/><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</iframe></template>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Nested templates remain inert</title>"
            "<template><template>inner</template>"
            "<iframe src=\"https://rumble.com/embed/vdecoy/\"></iframe>"
            "</template>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Template comment close is inert</title>"
            "<template><!-- </template> -->"
            "<iframe src=\"https://rumble.com/embed/vdecoy/\"></iframe>"
            "</template>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Template script close is inert</title>"
            "<template><script>const close = '</template>';</script>"
            "<iframe src=\"https://rumble.com/embed/vdecoy/\"></iframe>"
            "</template>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Source-less iframe fallback is inert</title>"
            "<iframe/><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</iframe>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Plaintext is inert</title>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\"></iframe>"
            "<plaintext><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</html>"),
    };

    for (const auto &body : livePages)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = body,
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("scoped"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    }

    const std::vector<QByteArray> offlinePages = {
        QByteArrayLiteral(
            "<html><title>Only ambiguous JSON</title>"
            "<script type=\"application/json\">"
            "{\"embedUrl\":\"https://rumble.com/embed/vone/\","
            "\"embed\\u0055rl\":\"https://rumble.com/embed/vtwo/\"}"
            "</script></html>"),
        QByteArrayLiteral(
            "<html><title>Only plaintext decoy</title>"
            "<plaintext><iframe src=\"https://rumble.com/embed/vdecoy/\">"),
    };
    for (const auto &body : offlinePages)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = body,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("offline"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
        EXPECT_EQ(transport.requestCount(), 1U);
    }
}

TEST(RumbleResolverParser, RejectsAmbiguousOrUnterminatedMarkup)
{
    const std::vector<QByteArray> malformed = {
        QByteArrayLiteral(
            "<html><title>Duplicate source</title>"
            "<iframe src=\"https://rumble.com/embed/vfixture/\" "
            "src=\"https://rumble.com/embed/vother/\"></iframe></html>"),
        QByteArrayLiteral(
            "<html><title>Unterminated comment</title>"
            "<!-- <iframe src=\"https://rumble.com/embed/vfixture/\">"),
        QByteArrayLiteral("<html><title>Unterminated script</title>"
                          "<script>const fake = true;</html>"),
        QByteArrayLiteral(
            "<html><title>Unterminated self-closing xmp</title>"
            "<xmp/><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Unterminated self-closing template</title>"
            "<template/><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</html>"),
        QByteArrayLiteral(
            "<html><title>Unterminated source-less iframe</title>"
            "<iframe/><iframe src=\"https://rumble.com/embed/vdecoy/\">"
            "</html>"),
    };

    for (const auto &body : malformed)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = body,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("malformed"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
    }
}

TEST(RumbleResolverParser, DistinguishesOfflineInterstitialAndAmbiguity)
{
    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><title>Normal channel</title><body>"
                    "<script>const text='Just a moment and vfake';</script>"
                    "<p>offline</p></body></html>"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("offline"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><title>Just a moment...</title>"
                    "<form id=\"challenge-form\"></form></html>"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("blocked"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::AccessInterstitial);
        ASSERT_TRUE(result->error);
        EXPECT_EQ(result->error->code, QStringLiteral("page_interstitial"));
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><title>Ambiguous</title>"
                    "<iframe src=\"https://rumble.com/embed/vone/\"></iframe>"
                    "<iframe src=\"https://rumble.com/embed/vtwo/\"></iframe>"
                    "</html>"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("ambiguous"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
    }
}

TEST(RumbleResolverParser, BodyAndSvgTitlesDoNotAmbiguateDocumentTitle)
{
    constexpr qsizetype candidateLimit = 4096;
    const QByteArray padding(candidateLimit + 1, 'x');
    const auto offlinePage =
        QByteArrayLiteral("<html><head><title>Offline channel</title></head>"
                          "<body data-hydration=\"") +
        padding +
        QByteArrayLiteral("\"><svg><title>Decorative channel icon</title></svg>"
                          "<title data-label=\"") +
        padding + QByteArrayLiteral("\">Body fallback</title></body></html>");
    {
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = offlinePage,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("offline"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><head><title>Live channel</title></head><body>"
                    "<svg><title>Decorative player icon</title></svg>"
                    "<script type=\"application/json\">"
                    "{\"embedUrl\":"
                    "\"https://rumble.com/embed/vfixture/\"}"
                    "</script></body></html>"),
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("live"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
    }

    const std::vector<QByteArray> malformed = {
        QByteArrayLiteral(
            "<html><body><svg><title>Not a document title</title></svg>"
            "</body></html>"),
        QByteArrayLiteral(
            "<html><head><title>First</title><title>Second</title></head>"
            "<body><svg><title>Ignored</title></svg></body></html>"),
        QByteArrayLiteral("<html><head><title>Document</title></head><body>"
                          "<svg><title>Unterminated</svg></body></html>"),
    };
    for (const auto &body : malformed)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({{
            .response = head(200, QByteArrayLiteral("text/html")),
            .body = body,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("malformed"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
    }
}

TEST(RumbleResolverSchema, RequiresLosslessIdAndTitles)
{
    const std::vector<QByteArray> malformed = {
        QByteArrayLiteral(R"({"vid":1001})"),
        QByteArrayLiteral(R"({"vid":"1.5","title":"bad"})"),
        QByteArrayLiteral(R"({"vid":1001,"title":7})"),
        QByteArrayLiteral(R"({"vid":0,"title":"bad"})"),
        QByteArrayLiteral(R"({"vid":999,"vid":1001,"title":"duplicate"})"),
        QByteArrayLiteral(
            R"({"vid":999,"\u0076id":1001,"title":"escaped duplicate"})"),
        QByteArrayLiteral(R"({"vid":1001,"title":"first","title":"second"})"),
        QByteArrayLiteral(
            R"({"vid":null,"unavailable":true,"unavailable":false})"),
        QByteArrayLiteral(R"({"title":"first","title":"second"})"),
        QByteArrayLiteral(
            R"({"vid":1001,"title":"bad channel","channel_id":"not-decimal"})"),
        QByteArrayLiteral(R"([])"),
    };

    for (const auto &body : malformed)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({
            {
                .expectedUrl = embedUrl(),
                .response = head(200, QByteArrayLiteral("application/json")),
                .body = body,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
    }

    ImmediateTransport transport({
        {
            .expectedUrl = embedUrl(),
            .response = head(200, QByteArrayLiteral("application/json")),
            .body = QByteArrayLiteral(
                R"({"vid":"999999999999999999999999999999","title":"Lossless","future":{"ok":true}})"),
        },
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<ResolveResult> result;
    auto handle =
        api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
            result = std::move(value);
        });
    queue.runAll();
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->metadata);
    EXPECT_EQ(result->metadata->streamId,
              QStringLiteral("999999999999999999999999999999"));

    ImmediateTransport nestedTransport({
        {
            .expectedUrl = embedUrl(),
            .response = head(200, QByteArrayLiteral("application/json")),
            .body = QByteArrayLiteral(
                R"({"nested":{"vid":999,"channel_id":888},)"
                R"("vid":1001,"title":"Top level","channel_id":777})"),
        },
    });
    DeferredQueue nestedQueue;
    RumbleApi nestedApi(nestedTransport, nestedQueue.dispatcher());
    std::optional<ResolveResult> nestedResult;
    auto nestedHandle =
        nestedApi.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
            nestedResult = std::move(value);
        });
    nestedQueue.runAll();
    ASSERT_TRUE(nestedResult);
    ASSERT_TRUE(nestedResult->metadata);
    EXPECT_EQ(nestedResult->metadata->streamId, QStringLiteral("1001"));
    EXPECT_EQ(nestedResult->metadata->channelIdentity, QStringLiteral("777"));
}

TEST(RumbleResolverSchema, TreatsAbsentOrNullVidAsOffline)
{
    const std::vector<QByteArray> offline = {
        QByteArrayLiteral(R"({})"),
        QByteArrayLiteral(R"({"title":"Unavailable"})"),
        QByteArrayLiteral(R"({"vid":null})"),
        QByteArrayLiteral(R"({"vid":null,"unavailable":false})"),
    };

    for (const auto &body : offline)
    {
        SCOPED_TRACE(body);
        ImmediateTransport transport({{
            .expectedUrl = embedUrl(),
            .response = head(200, QByteArrayLiteral("application/json")),
            .body = body,
        }});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
        ASSERT_TRUE(result->metadata);
        EXPECT_EQ(result->metadata->embedId, QStringLiteral("vfixture"));
        EXPECT_TRUE(result->metadata->streamId.isEmpty());
    }
}

TEST(RumbleResolverMetadata, DoesNotFabricateChannelFieldsForVideoInputs)
{
    {
        ImmediateTransport transport({validEmbed()});
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->metadata);
        EXPECT_TRUE(result->metadata->channelIdentity.isEmpty());
        EXPECT_TRUE(result->metadata->channelTitle.isEmpty());
        EXPECT_EQ(result->metadata->videoTitle,
                  QStringLiteral("Fixture title"));
    }

    {
        ImmediateTransport transport({
            {
                .expectedUrl = QUrl(QStringLiteral(
                    "https://rumble.com/vfixture-synthetic-title.html")),
                .response = head(200, QByteArrayLiteral("text/html")),
                .body = QByteArrayLiteral(
                    "<html><title>Video page title</title>"
                    "<iframe src=\"https://rumble.com/embed/vfixture/\">"
                    "</iframe></html>"),
            },
            validEmbed(),
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle = api.resolve(
            QStringLiteral("https://rumble.com/vfixture-synthetic-title.html"),
            [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->metadata);
        EXPECT_TRUE(result->metadata->channelIdentity.isEmpty());
        EXPECT_TRUE(result->metadata->channelTitle.isEmpty());
        EXPECT_EQ(result->metadata->videoTitle,
                  QStringLiteral("Fixture title"));
    }
}

TEST(RumbleResolverCallbacks, SynchronousTransportStillDefersExactlyOnce)
{
    ImmediateExchange exchange = validEmbed();
    exchange.duplicateTerminal = true;
    ImmediateTransport transport({std::move(exchange)});
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());

    int calls = 0;
    auto handle =
        api.resolve(QStringLiteral("vfixture"), [&](ResolveResult result) {
            ++calls;
            EXPECT_EQ(result.outcome, Outcome::ResolvedLive);
        });
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(queue.size(), 1U);
    queue.runAll();
    EXPECT_EQ(calls, 1);
}

TEST(RumbleResolverCallbacks, CancellationSuppressesQueuedCompletion)
{
    ImmediateTransport transport({});
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());

    int calls = 0;
    auto handle = api.resolve(QStringLiteral("1001"), [&](ResolveResult) {
        ++calls;
    });
    EXPECT_EQ(queue.size(), 1U);
    handle.cancel();
    queue.runAll();
    EXPECT_EQ(calls, 0);
}

TEST(RumbleResolverCallbacks, RaiiCancelAndLateCompletionAreSafe)
{
    ControlledTransport transport;
    DeferredQueue queue;
    int calls = 0;

    {
        RumbleApi api(transport, queue.dispatcher());
        {
            auto handle =
                api.resolve(QStringLiteral("vfixture"), [&](ResolveResult) {
                    ++calls;
                });
            EXPECT_TRUE(handle.active());
        }
        EXPECT_EQ(transport.cancelCount(), 1);
        transport.deliverLate();
        queue.runAll();
        EXPECT_EQ(calls, 0);
    }

    transport.deliverLate();
    queue.runAll();
    EXPECT_EQ(calls, 0);
}

TEST(RumbleResolverCallbacks, DuplicateResponseHeadFailsExactlyOnce)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    int calls = 0;
    std::optional<ResolveResult> result;
    auto handle =
        api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
            ++calls;
            result = std::move(value);
        });

    transport.deliverHead(head(200, QByteArrayLiteral("application/json")));
    transport.deliverHead(head(200, QByteArrayLiteral("application/json")));
    transport.complete();
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result->outcome, Outcome::TransportError);
    ASSERT_TRUE(result->error);
    EXPECT_EQ(result->error->code, QStringLiteral("invalid_response_head"));
}

TEST(RumbleResolverFailures, PreservesTypedHttpRetryAndTransportOutcomes)
{
    const QDateTime now(QDate(2026, 7, 15), QTime(1, 0, 0), Qt::UTC);
    {
        // HTTP status classification precedes success-representation media
        // validation. Error responses need not use the requested media type.
        auto response = head(429, QByteArrayLiteral("text/plain"));
        response.headers.push_back(
            {QByteArrayLiteral("Retry-After"), QByteArrayLiteral("3")});
        ImmediateTransport transport({
            {.response = std::move(response),
             .body = QByteArrayLiteral(R"({"error":"ignored"})")},
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher(), [now] {
            return now;
        });
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::RateLimited);
        ASSERT_TRUE(result->error);
        EXPECT_EQ(result->error->httpStatus, 429);
        EXPECT_TRUE(result->error->retry.retryable);
        ASSERT_TRUE(result->error->retry.after);
        EXPECT_EQ(*result->error->retry.after, std::chrono::seconds(3));
    }

    {
        auto response = head(429, QByteArrayLiteral("text/html"));
        response.headers.push_back(
            {QByteArrayLiteral("Retry-After"),
             QByteArrayLiteral("Wed, 15 Jul 2026 01:00:10 GMT")});
        ImmediateTransport transport({
            {.response = std::move(response),
             .body = QByteArrayLiteral("rate limited")},
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher(), [now] {
            return now;
        });
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::RateLimited);
        ASSERT_TRUE(result->error);
        ASSERT_TRUE(result->error->retry.after);
        EXPECT_EQ(*result->error->retry.after, std::chrono::seconds(10));
    }

    {
        auto response = head(429, QByteArrayLiteral("text/plain"));
        response.headers.push_back(
            {QByteArrayLiteral("Retry-After"), QByteArrayLiteral("3")});
        ImmediateTransport transport({
            {.response = std::move(response)},
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher(), []() -> QDateTime {
            throw std::runtime_error("clock-canary-secret");
        });
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::RateLimited);
        ASSERT_TRUE(result->error);
        EXPECT_TRUE(result->error->retry.retryable);
        EXPECT_FALSE(result->error->retry.after);
    }

    struct HttpCase {
        int status;
        Outcome outcome;
        bool retryable;
    };
    for (const auto &test : std::vector<HttpCase>{
             {404, Outcome::NotFound, false},
             {503, Outcome::HttpError, true},
         })
    {
        SCOPED_TRACE(test.status);
        ImmediateTransport transport({
            {
                .response = head(test.status, QByteArrayLiteral("text/plain")),
                .body = QByteArrayLiteral("error"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, test.outcome);
        ASSERT_TRUE(result->error);
        EXPECT_EQ(result->error->httpStatus, test.status);
        EXPECT_EQ(result->error->retry.retryable, test.retryable);
    }

    struct TransportCase {
        TransportFailure failure;
        Outcome outcome;
        bool retryable;
    };
    const std::vector<TransportCase> failures = {
        {TransportFailure::Timeout, Outcome::Timeout, true},
        {TransportFailure::Cancelled, Outcome::Cancelled, false},
        {TransportFailure::OwnerDestroyed, Outcome::Cancelled, false},
        {TransportFailure::RedirectRejected, Outcome::RedirectRejected, false},
        {TransportFailure::BodyLimit, Outcome::LimitExceeded, false},
        {TransportFailure::HeaderLimit, Outcome::LimitExceeded, false},
        {TransportFailure::InvalidMediaType, Outcome::InvalidMediaType, false},
        {TransportFailure::Network, Outcome::TransportError, true},
    };
    for (const auto &test : failures)
    {
        ImmediateTransport transport({
            {
                .response = {},
                .failure = test.failure,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, test.outcome);
        ASSERT_TRUE(result->error);
        EXPECT_EQ(result->error->retry.retryable, test.retryable);
    }
}

TEST(RumbleApiTransport, BodyLimitDiagnosticsIdentifyRequestStage)
{
    const std::vector<std::pair<QString, QString>> resolveCases = {
        {QStringLiteral("https://rumble.com/vfixture-page.html"),
         QStringLiteral("page_body_limit")},
        {QStringLiteral("vfixture"), QStringLiteral("embed_body_limit")},
    };
    for (const auto &[input, expectedCode] : resolveCases)
    {
        SCOPED_TRACE(input);
        ImmediateTransport transport({
            {
                .response = {},
                .failure = TransportFailure::BodyLimit,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle = api.resolve(input, [&](ResolveResult value) {
            result = std::move(value);
        });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::LimitExceeded);
        ASSERT_TRUE(result->error);
        EXPECT_EQ(result->error->code, expectedCode);
    }

    ImmediateTransport transport({
        {
            .response = {},
            .failure = TransportFailure::BodyLimit,
        },
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<StreamTerminal> terminal;
    auto handle = api.stream(QStringLiteral("1001"),
                             {
                                 .onEvents = [](StreamBatch) {},
                                 .onTerminal =
                                     [&](StreamTerminal value) {
                                         terminal = std::move(value);
                                     },
                             });
    queue.runAll();
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->outcome, Outcome::LimitExceeded);
    ASSERT_TRUE(terminal->error);
    EXPECT_EQ(terminal->error->code, QStringLiteral("sse_body_limit"));
}

TEST(RumbleResolverLimits, EnforcesMediaBodyAndHeaderBoundsWithFakeTransport)
{
    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/plain")),
                .body = QByteArrayLiteral("{}"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::InvalidMediaType);
    }

    {
        auto response = head(200, QByteArrayLiteral("application/json"));
        response.headers.push_back({QByteArrayLiteral("content-type"),
                                    QByteArrayLiteral("application/json")});
        ImmediateTransport transport({
            {.response = std::move(response), .body = QByteArrayLiteral("{}")},
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::InvalidMediaType);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("application/json")),
                .body = QByteArray(300 * 1024, 'x'),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::LimitExceeded);
    }

    {
        auto response = head(200, QByteArrayLiteral("application/json"));
        for (int index = 0; index < MAX_RESPONSE_HEADERS; ++index)
        {
            response.headers.push_back(
                {QByteArray("X-Fixture-") + QByteArray::number(index),
                 QByteArrayLiteral("x")});
        }
        ImmediateTransport transport({
            {.response = std::move(response), .body = QByteArrayLiteral("{}")},
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<ResolveResult> result;
        auto handle =
            api.resolve(QStringLiteral("vfixture"), [&](ResolveResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::LimitExceeded);
    }
}

TEST(RumbleApiBootstrap, SynchronousBodyWinsOverQueuedCleanEof)
{
    ImmediateTransport transport({
        {
            .response = head(200, QByteArrayLiteral("text/event-stream")),
            .body =
                QByteArrayLiteral("data: {\"type\":\"init\",\"data\":{}}\n\n"),
        },
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<BootstrapResult> result;
    int callbackCount = 0;
    bool bootstrapReturned = false;
    bool callbackSawReturn = false;

    auto handle =
        api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
            ++callbackCount;
            callbackSawReturn = bootstrapReturned;
            result = std::move(value);
        });

    EXPECT_EQ(callbackCount, 0);
    EXPECT_FALSE(result);
    bootstrapReturned = true;
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_TRUE(callbackSawReturn);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_EQ(result->events.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<InitEvent>(result->events.front()));

    queue.runAll();
    EXPECT_EQ(callbackCount, 1);
}

TEST(RumbleApiBootstrap, ParsesBoundedEventsAndObservedMediaType)
{
    const QByteArray valid = "data: {\"type\":\"init\",\"data\":{}}\n\n"
                             "event: update\n"
                             "data: {\"type\":\"messages\",\"data\":{}}\n\n";

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/event-stream")),
                .body = valid,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<BootstrapResult> result;
        auto handle =
            api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_EQ(result->events.size(), 2U);
        EXPECT_TRUE(std::holds_alternative<InitEvent>(result->events[0]));
        EXPECT_TRUE(std::holds_alternative<MessagesEvent>(result->events[1]));
        EXPECT_TRUE(result->diagnostics.empty());
    }

    {
        ImmediateTransport transport({
            {
                .response = head(
                    200,
                    QByteArrayLiteral(
                        "text/event-stream; charset=utf-8; charset=UTF-8")),
                .body = valid,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<BootstrapResult> result;
        auto handle =
            api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
        ASSERT_EQ(result->events.size(), 2U);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(
                    200,
                    QByteArrayLiteral("text/event-stream; charset=iso-8859-1")),
                .body = valid,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<BootstrapResult> result;
        auto handle =
            api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::InvalidMediaType);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/event-stream")),
                .body = QByteArrayLiteral("data: {not-json}\n\n"),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<BootstrapResult> result;
        auto handle =
            api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::MalformedSchema);
    }

    {
        ImmediateTransport transport({
            {
                .response = head(204, {}),
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<BootstrapResult> result;
        auto handle =
            api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::ValidOffline);
    }
}

TEST(RumbleApiBootstrap, ExposesTypedEventsAndSanitizedDiagnosticsOnly)
{
    ImmediateTransport transport({
        {
            .response = head(200, QByteArrayLiteral("text/event-stream")),
            .body = QByteArrayLiteral(
                "data: {not-json}\n\n"
                "data: {\"type\":\"event-canary-secret\",\"data\":{}}\n\n"
                "data: {\"type\":\"init\",\"data\":{}}\n\n"),
        },
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<BootstrapResult> result;
    auto handle =
        api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
            result = std::move(value);
        });
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_EQ(result->events.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<InitEvent>(result->events.front()));
    ASSERT_EQ(result->diagnostics.size(), 2U);
    EXPECT_EQ(result->diagnostics[0].code, QStringLiteral("malformed_json"));
    EXPECT_EQ(result->diagnostics[0].path, QStringLiteral("$"));
    EXPECT_EQ(result->diagnostics[1].code,
              QStringLiteral("unknown_event_type"));
    EXPECT_EQ(result->diagnostics[1].path, QStringLiteral("type"));
    for (const auto &diagnostic : result->diagnostics)
    {
        EXPECT_FALSE(
            diagnostic.code.contains(QStringLiteral("event-canary-secret")));
        EXPECT_FALSE(
            diagnostic.path.contains(QStringLiteral("event-canary-secret")));
    }
}

TEST(RumbleApiBootstrap, HandsOffCompleteFramesWithoutWaitingForDisconnect)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    int calls = 0;
    std::optional<BootstrapResult> result;
    auto handle =
        api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
            ++calls;
            result = std::move(value);
        });

    transport.deliverHead(head(200, QByteArrayLiteral("text/event-stream")));
    transport.deliverBody(QByteArrayLiteral(": keepalive\n\n"));
    EXPECT_EQ(queue.size(), 0U);

    transport.deliverBody(
        QByteArrayLiteral("event: init\r\n"
                          "data: {\"type\":\"init\",\"data\":{}}\r\n"));
    EXPECT_EQ(queue.size(), 0U);
    transport.deliverBody(QByteArrayLiteral("\r\ndata: {\"partial\":"));
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_EQ(transport.cancelCount(), 1);
    EXPECT_FALSE(result);

    // Even a deliberately non-conforming late terminal is ignored.
    transport.fail(TransportFailure::Timeout);
    queue.runAll();
    ASSERT_TRUE(result);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_EQ(result->events.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<InitEvent>(result->events.front()));
    EXPECT_TRUE(result->diagnostics.empty());
}

TEST(RumbleApiBootstrap, IncompleteFrameRemainsPendingUntilTypedFailure)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<BootstrapResult> result;
    auto handle =
        api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
            result = std::move(value);
        });

    transport.deliverHead(head(200, QByteArrayLiteral("text/event-stream")));
    transport.deliverBody(QByteArrayLiteral("data: {\"type\":\"init\"}\n"));
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_FALSE(result);

    transport.fail(TransportFailure::Timeout);
    queue.runAll();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::Timeout);
    ASSERT_TRUE(result->error);
    EXPECT_EQ(result->error->code, QStringLiteral("transport_timeout"));
}

TEST(RumbleApiBootstrap, RejectsEventCountAndEventSizeLimits)
{
    QByteArray tooMany;
    for (int index = 0; index < 65; ++index)
    {
        tooMany.append("data: {}\n\n");
    }
    const std::vector<std::pair<QByteArray, QString>> invalidBodies = {
        {tooMany, QStringLiteral("sse_event_count_limit")},
        {QByteArrayLiteral("data: {\"value\":\"") +
             QByteArray(1024 * 1024, 'x') + QByteArrayLiteral("\"}\n\n"),
         QStringLiteral("sse_event_size_limit")},
    };

    for (const auto &[body, expectedCode] : invalidBodies)
    {
        SCOPED_TRACE(body.size());
        ImmediateTransport transport({
            {
                .response = head(200, QByteArrayLiteral("text/event-stream")),
                .body = body,
            },
        });
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::optional<BootstrapResult> result;
        auto handle =
            api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
                result = std::move(value);
            });
        queue.runAll();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, Outcome::LimitExceeded);
        ASSERT_TRUE(result->error);
        EXPECT_EQ(result->error->code, expectedCode);
    }
}

TEST(RumbleApiBootstrap, RejectsOversizedUnterminatedEventBeforeBodyLimit)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<BootstrapResult> result;
    auto handle =
        api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
            result = std::move(value);
        });

    transport.deliverHead(head(200, QByteArrayLiteral("text/event-stream")));
    transport.deliverBody(QByteArrayLiteral("data: ") +
                          QByteArray(1024 * 1024 + 1, 'x'));
    queue.runAll();

    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::LimitExceeded);
    ASSERT_TRUE(result->error);
    EXPECT_EQ(result->error->code, QStringLiteral("sse_event_size_limit"));
}

TEST(RumbleApiBootstrap, AcceptsCurrentInitBeyondLegacy64KiBLimit)
{
    QByteArray body = QByteArrayLiteral(
        "data: {\"type\":\"init\",\"data\":{\"users\":[],\"channels\":[],"
        "\"config\":{\"badges\":{}},\"messages\":[],\"padding\":\"");
    body.append(QByteArray(96 * 1024, 'x'));
    body.append(QByteArrayLiteral("\"}}\n\n"));

    ImmediateTransport transport({
        {
            .response = head(200, QByteArrayLiteral("text/event-stream")),
            .body = std::move(body),
        },
    });
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<BootstrapResult> result;
    auto handle =
        api.bootstrap(QStringLiteral("1001"), [&](BootstrapResult value) {
            result = std::move(value);
        });

    queue.runAll();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_EQ(result->events.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<InitEvent>(result->events.front()));
    EXPECT_TRUE(result->diagnostics.empty());
}

TEST(RumbleApiEmoteCatalog, UsesBoundedPublicRequestAndTypedCatalog)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::optional<EmoteCatalogResult> result;
    auto cancellation =
        api.emoteCatalog(QStringLiteral("1001"), [&](EmoteCatalogResult value) {
            result = std::move(value);
        });

    EXPECT_EQ(
        transport.request().url,
        QUrl(QStringLiteral(
            "https://rumble.com/service.php?name=emote.list&chat_id=1001")));
    EXPECT_EQ(transport.request().expectedMediaType, ExpectedMediaType::Json);
    EXPECT_EQ(transport.request().maxBodyBytes,
              rumble::MAX_EMOTE_CATALOG_BYTES);
    ASSERT_EQ(transport.request().headers.size(), 2U);
    EXPECT_EQ(transport.request().headers[0].name, QByteArrayLiteral("Accept"));
    EXPECT_EQ(transport.request().headers[0].value,
              QByteArrayLiteral("application/json"));

    transport.deliverHead(head(200, QByteArrayLiteral("application/json")));
    transport.deliverBody(QByteArrayLiteral(
        R"({"data":{"items":[{"id":"global","channel_id":null,"emotes":[{"name":"r+wave","is_subs_only":false,"position":1,"file":"https://1a-1791.com/video/z12/wave.png"}]}]}})"));
    transport.complete();
    queue.runAll();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, Outcome::ResolvedLive);
    ASSERT_TRUE(result->catalog);
    ASSERT_EQ(result->catalog->emotes.size(), 1U);
    EXPECT_EQ(result->catalog->emotes[0].id, QStringLiteral("global:1"));
}

TEST(RumbleApiEmoteCatalog, InvalidInputAndSchemaFailOnceWithoutRawData)
{
    DeferredQueue queue;
    ImmediateTransport unused({});
    RumbleApi invalidApi(unused, queue.dispatcher());
    std::optional<EmoteCatalogResult> invalid;
    auto invalidCancellation = invalidApi.emoteCatalog(
        QStringLiteral("not-a-stream"), [&](EmoteCatalogResult value) {
            invalid = std::move(value);
        });
    queue.runAll();
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->outcome, Outcome::UnsupportedInput);
    EXPECT_EQ(unused.requestCount(), 0U);

    ImmediateTransport transport({{
        .response = head(200, QByteArrayLiteral("application/json")),
        .body = QByteArrayLiteral("{\"private-canary\":true}"),
        .duplicateTerminal = true,
    }});
    RumbleApi api(transport, queue.dispatcher());
    int calls = 0;
    std::optional<EmoteCatalogResult> malformed;
    auto cancellation =
        api.emoteCatalog(QStringLiteral("1001"), [&](EmoteCatalogResult value) {
            ++calls;
            malformed = std::move(value);
        });
    queue.runAll();
    ASSERT_TRUE(malformed);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(malformed->outcome, Outcome::MalformedSchema);
    ASSERT_TRUE(malformed->error);
    EXPECT_EQ(malformed->error->code, QStringLiteral("emote_catalog_schema"));
    EXPECT_FALSE(
        malformed->error->code.contains(QStringLiteral("private-canary")));
}

TEST(RumbleApiStream, UsesPersistentScopesAndOrdersBatchesBeforeCleanEof)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::vector<QString> order;
    std::size_t eventCount = 0;
    std::optional<StreamTerminal> terminal;
    auto cancellation =
        api.stream(QStringLiteral("1001"),
                   {
                       .onEvents =
                           [&](StreamBatch batch) {
                               order.push_back(QStringLiteral("batch"));
                               eventCount += batch.events.size();
                           },
                       .onTerminal =
                           [&](StreamTerminal value) {
                               order.push_back(QStringLiteral("terminal"));
                               terminal = std::move(value);
                           },
                   });
    EXPECT_EQ(transport.request().deadlineScope, DeadlineScope::UntilFinalHead);
    EXPECT_EQ(transport.request().bodyLimitScope,
              BodyLimitScope::PendingDelivery);
    ASSERT_EQ(transport.request().headers.size(), 5U);
    EXPECT_EQ(transport.request().headers[0].name, QByteArrayLiteral("Accept"));
    EXPECT_EQ(transport.request().headers[0].value,
              QByteArrayLiteral("text/event-stream"));
    EXPECT_EQ(transport.request().headers[1].name,
              QByteArrayLiteral("Cache-Control"));
    EXPECT_EQ(transport.request().headers[1].value,
              QByteArrayLiteral("no-cache"));
    EXPECT_EQ(transport.request().headers[2].name, QByteArrayLiteral("Origin"));
    EXPECT_EQ(transport.request().headers[2].value,
              QByteArrayLiteral("https://rumble.com"));
    EXPECT_EQ(transport.request().headers[3].name,
              QByteArrayLiteral("Referer"));
    EXPECT_EQ(transport.request().headers[3].value,
              QByteArrayLiteral("https://rumble.com/"));
    EXPECT_EQ(transport.request().headers[4].name,
              QByteArrayLiteral("User-Agent"));
    EXPECT_EQ(transport.request().headers[4].value,
              QByteArrayLiteral("chatterino-rumble/1"));

    transport.deliverHead(head(200, QByteArrayLiteral("text/event-stream")));
    transport.deliverBody(QByteArrayLiteral(
        "data: {\"type\":\"init\",\"data\":{\"messages\":[]}}\n\n"));
    transport.complete();
    EXPECT_TRUE(order.empty());
    queue.runAll();

    EXPECT_EQ(order, (std::vector<QString>{QStringLiteral("batch"),
                                           QStringLiteral("terminal")}));
    EXPECT_EQ(eventCount, 1U);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->outcome, Outcome::TransportError);
    ASSERT_TRUE(terminal->error);
    EXPECT_EQ(terminal->error->code, QStringLiteral("stream_eof"));
    EXPECT_TRUE(terminal->error->retry.retryable);
}

TEST(RumbleApiStream, CleanEofAcceptsEmptyCommentAndPartialTails)
{
    const std::vector<QByteArray> tails = {
        {},
        QByteArrayLiteral(": comment without newline"),
        QByteArrayLiteral("data: {\"partial\":"),
        QByteArrayLiteral(
            "data: {\"type\":\"init\",\"data\":{\"messages\":[]}}\n\n"
            "data: partial"),
    };
    for (const auto &tail : tails)
    {
        SCOPED_TRACE(tail);
        ControlledTransport transport;
        DeferredQueue queue;
        RumbleApi api(transport, queue.dispatcher());
        std::vector<QString> order;
        std::optional<StreamTerminal> terminal;
        auto cancellation =
            api.stream(QStringLiteral("1001"),
                       {
                           .onEvents =
                               [&](StreamBatch) {
                                   order.push_back(QStringLiteral("batch"));
                               },
                           .onTerminal =
                               [&](StreamTerminal value) {
                                   order.push_back(QStringLiteral("terminal"));
                                   terminal = std::move(value);
                               },
                       });
        transport.deliverHead(
            head(200, QByteArrayLiteral("text/event-stream")));
        if (!tail.isEmpty())
            transport.deliverBody(tail);
        transport.complete();
        queue.runAll();
        ASSERT_TRUE(terminal);
        ASSERT_TRUE(terminal->error);
        EXPECT_EQ(terminal->error->code, QStringLiteral("stream_eof"));
        EXPECT_EQ(order.back(), QStringLiteral("terminal"));
    }
}

TEST(RumbleApiStream, HasNoInventedPostHeadHeartbeatDeadline)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    int terminals = 0;
    auto cancellation =
        api.stream(QStringLiteral("1001"), {
                                               .onEvents = [](StreamBatch) {},
                                               .onTerminal =
                                                   [&](StreamTerminal) {
                                                       ++terminals;
                                                   },
                                           });
    transport.deliverHead(head(200, QByteArrayLiteral("text/event-stream")));
    queue.runAll();
    EXPECT_TRUE(cancellation.active());
    EXPECT_EQ(terminals, 0);
    cancellation.cancel();
    queue.runAll();
    EXPECT_EQ(terminals, 0);
}

TEST(RumbleApiStream, BoundsQueuedTypedHandoffs)
{
    ControlledTransport transport;
    DeferredQueue queue;
    RumbleApi api(transport, queue.dispatcher());
    std::size_t batches = 0;
    std::optional<StreamTerminal> terminal;
    auto cancellation = api.stream(QStringLiteral("1001"),
                                   {
                                       .onEvents =
                                           [&](StreamBatch) {
                                               ++batches;
                                           },
                                       .onTerminal =
                                           [&](StreamTerminal value) {
                                               terminal = std::move(value);
                                           },
                                   });
    transport.deliverHead(head(200, QByteArrayLiteral("text/event-stream")));
    for (int index = 0; index < 9; ++index)
    {
        transport.deliverBody(QByteArrayLiteral(
            "data: {\"type\":\"init\",\"data\":{\"messages\":[]}}\n\n"));
    }
    EXPECT_EQ(transport.cancelCount(), 1);
    queue.runAll();
    EXPECT_EQ(batches, 8U);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->outcome, Outcome::LimitExceeded);
    ASSERT_TRUE(terminal->error);
    EXPECT_EQ(terminal->error->code, QStringLiteral("stream_handoff_limit"));
}

TEST(RumbleTransportQt, AppliesFixedPublicContextAndCredentialFreeAttributes)
{
    const auto request = validSseRequest();
    const auto prepared = RumbleQtTransport::prepareRequest(request);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared->url(), request.url);
    EXPECT_EQ(
        prepared->attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
        static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
    EXPECT_EQ(prepared->attribute(QNetworkRequest::CookieLoadControlAttribute)
                  .toInt(),
              static_cast<int>(QNetworkRequest::Manual));
    EXPECT_EQ(prepared->attribute(QNetworkRequest::CookieSaveControlAttribute)
                  .toInt(),
              static_cast<int>(QNetworkRequest::Manual));
    EXPECT_EQ(prepared->attribute(QNetworkRequest::AuthenticationReuseAttribute)
                  .toInt(),
              static_cast<int>(QNetworkRequest::Manual));
    EXPECT_EQ(
        prepared->attribute(QNetworkRequest::CacheLoadControlAttribute).toInt(),
        static_cast<int>(QNetworkRequest::AlwaysNetwork));
    EXPECT_FALSE(prepared->attribute(QNetworkRequest::CacheSaveControlAttribute)
                     .toBool());
    EXPECT_FALSE(
        prepared->attribute(QNetworkRequest::AutoDeleteReplyOnFinishAttribute)
            .toBool());
    EXPECT_EQ(prepared->maximumRedirectsAllowed(), 3);
    EXPECT_FALSE(prepared->hasRawHeader("Cookie"));
    EXPECT_FALSE(prepared->hasRawHeader("Authorization"));
    EXPECT_EQ(prepared->rawHeader("Origin"),
              QByteArrayLiteral("https://rumble.com"));
    EXPECT_EQ(prepared->rawHeader("Referer"),
              QByteArrayLiteral("https://rumble.com/"));
    EXPECT_EQ(prepared->rawHeader("User-Agent"),
              QByteArrayLiteral("chatterino-rumble/1"));

    auto foreign = request;
    foreign.url.setHost(QStringLiteral("example.invalid"));
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(foreign));

    auto encodedSlash = request;
    encodedSlash.url = QUrl(QStringLiteral(
        "https://web7.rumble.com/chat/api/chat/1001%2Fother/stream"));
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(encodedSlash));

    auto emptyQuery = request;
    emptyQuery.url = QUrl(
        QStringLiteral("https://web7.rumble.com/chat/api/chat/1001/stream?"));
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(emptyQuery));

    auto emptyFragment = request;
    emptyFragment.url = QUrl(
        QStringLiteral("https://web7.rumble.com/chat/api/chat/1001/stream#"));
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(emptyFragment));

    auto credentialHeader = request;
    credentialHeader.headers.push_back({QByteArrayLiteral("Authorization"),
                                        QByteArrayLiteral("canary-secret")});
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(credentialHeader));

    auto cookieHeader = request;
    cookieHeader.headers.push_back(
        {QByteArrayLiteral("Cookie"), QByteArrayLiteral("canary-secret")});
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(cookieHeader));

    auto originHeader = request;
    originHeader.headers[2].value =
        QByteArrayLiteral("https://example.invalid");
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(originHeader));

    auto refererHeader = request;
    refererHeader.headers[3].value =
        QByteArrayLiteral("https://rumble.com/watch-secret");
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(refererHeader));

    auto duplicateAccept = request;
    duplicateAccept.headers.push_back(
        {QByteArrayLiteral("Accept"), QByteArrayLiteral("text/event-stream")});
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(duplicateAccept));

    auto wrongUserAgent = request;
    wrongUserAgent.headers.back().value =
        QByteArrayLiteral("browser-canary-secret");
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(wrongUserAgent));

    auto duplicateUserAgent = request;
    duplicateUserAgent.headers.push_back(
        {QByteArrayLiteral("User-Agent"),
         QByteArrayLiteral("chatterino-rumble/1")});
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(duplicateUserAgent));

    auto missingAccept = request;
    missingAccept.headers.erase(missingAccept.headers.begin());
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(missingAccept));

    auto missingOrigin = request;
    missingOrigin.headers.erase(missingOrigin.headers.begin() + 2);
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(missingOrigin));

    auto missingReferer = request;
    missingReferer.headers.erase(missingReferer.headers.begin() + 3);
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(missingReferer));

    auto missingUserAgent = request;
    missingUserAgent.headers.pop_back();
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(missingUserAgent));
}

TEST(RumbleTransportQt, AllowsOnlyExactPublicEmoteCatalogQuery)
{
    const auto request = validEmoteCatalogRequest();
    const auto prepared = RumbleQtTransport::prepareRequest(request);
    ASSERT_TRUE(prepared);
    EXPECT_FALSE(prepared->hasRawHeader("Cookie"));
    EXPECT_FALSE(prepared->hasRawHeader("Authorization"));

    for (const auto &url : {
             QStringLiteral(
                 "https://rumble.com/service.php?chat_id=1001&name=emote.list"),
             QStringLiteral(
                 "https://rumble.com/service.php?name=other&chat_id=1001"),
             QStringLiteral(
                 "https://rumble.com/service.php?name=emote.list&chat_id=0"),
             QStringLiteral("https://rumble.com/"
                            "service.php?name=emote.list&chat_id=1001&extra=1"),
             QStringLiteral("https://web7.rumble.com/"
                            "service.php?name=emote.list&chat_id=1001"),
         })
    {
        auto rejected = request;
        rejected.url = QUrl(url);
        EXPECT_FALSE(RumbleQtTransport::prepareRequest(rejected)) << url;
    }
}

TEST(RumbleTransportQt, RestrictsPersistentScopesToEventStreams)
{
    auto persistent = validSseRequest();
    persistent.deadlineScope = DeadlineScope::UntilFinalHead;
    persistent.bodyLimitScope = BodyLimitScope::PendingDelivery;
    ASSERT_TRUE(RumbleQtTransport::prepareRequest(persistent));

    auto finite = validHtmlRequest();
    finite.deadlineScope = DeadlineScope::UntilFinalHead;
    finite.bodyLimitScope = BodyLimitScope::PendingDelivery;
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(finite));

    auto crossed = persistent;
    crossed.bodyLimitScope = BodyLimitScope::Cumulative;
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(crossed));

    auto invalid = persistent;
    invalid.deadlineScope = static_cast<DeadlineScope>(255);
    EXPECT_FALSE(RumbleQtTransport::prepareRequest(invalid));
}

TEST(RumbleTransportQt, OrdersTerminalCallbacksAndClassifiesReplyErrors)
{
    {
        // Keep bytes unread until finished() to exercise replyFinished's final
        // drain rather than the ordinary readyRead path.
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream; charset=utf-8; "
                                           "charset=UTF-8")}},
            .body = QByteArrayLiteral("partial response"),
            .notifyReadyRead = false,
            .error = QNetworkReply::RemoteHostClosedError,
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(validSseRequest(), log.callbacks());
        runQtEventLoop();

        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("head"),
                                                   QStringLiteral("body"),
                                                   QStringLiteral("failure")}));
        EXPECT_EQ(log.body, QByteArrayLiteral("partial response"));
        EXPECT_EQ(log.heads, 1);
        EXPECT_EQ(log.completions, 0);
        EXPECT_EQ(log.failures, 1);
        ASSERT_TRUE(log.failure);
        EXPECT_EQ(*log.failure, TransportFailure::Network);
    }

    {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .error = QNetworkReply::TimeoutError,
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(validSseRequest(), log.callbacks());
        runQtEventLoop();

        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("head"),
                                                   QStringLiteral("failure")}));
        EXPECT_EQ(log.failures, 1);
        ASSERT_TRUE(log.failure);
        EXPECT_EQ(*log.failure, TransportFailure::Timeout);
        EXPECT_EQ(log.completions, 0);
    }

    {
        // Qt surfaces HTTP failures through QNetworkReply::error(). The
        // transport must still complete so RumbleApi can map the status.
        FakeNetworkAccessManager manager({{
            .status = 404,
            .body = QByteArrayLiteral("not found"),
            .error = QNetworkReply::ContentNotFoundError,
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(validSseRequest(), log.callbacks());
        runQtEventLoop();

        EXPECT_EQ(log.order, (std::vector<QString>{
                                 QStringLiteral("head"), QStringLiteral("body"),
                                 QStringLiteral("complete")}));
        EXPECT_EQ(log.heads, 1);
        EXPECT_EQ(log.completions, 1);
        EXPECT_EQ(log.failures, 0);
    }

    {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .body = QByteArrayLiteral("complete response"),
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(validSseRequest(), log.callbacks());
        runQtEventLoop();

        EXPECT_EQ(log.order, (std::vector<QString>{
                                 QStringLiteral("head"), QStringLiteral("body"),
                                 QStringLiteral("complete")}));
        EXPECT_EQ(log.heads, 1);
        EXPECT_EQ(log.completions, 1);
        EXPECT_EQ(log.failures, 0);
    }
}

TEST(RumbleTransportQt, TimesOutAndSuppressesExplicitlyCancelledReplies)
{
    {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .finish = false,
        }});
        RumbleQtTransport transport(manager, nullptr);
        auto request = validSseRequest();
        request.timeoutMs = 1;
        QtCallbackLog log;
        auto handle = transport.start(std::move(request), log.callbacks());
        runQtEventLoop(20);

        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("head"),
                                                   QStringLiteral("failure")}));
        EXPECT_EQ(log.failures, 1);
        ASSERT_TRUE(log.failure);
        EXPECT_EQ(*log.failure, TransportFailure::Timeout);
        EXPECT_EQ(log.completions, 0);
    }

    {
        auto abortCount = std::make_shared<std::atomic<int>>(0);
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .body = QByteArrayLiteral("must not escape cancellation"),
            .abortCount = abortCount,
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(validSseRequest(), log.callbacks());
        handle->cancel();
        EXPECT_EQ(abortCount->load(std::memory_order_relaxed), 1);
        EXPECT_FALSE(handle->active());
        runQtEventLoop();
        EXPECT_TRUE(log.order.empty());
        EXPECT_EQ(log.completions, 0);
        EXPECT_EQ(log.failures, 0);
    }
}

TEST(RumbleTransportQt, ForeignThreadReplyFailsClosedAndQueuesCleanup)
{
    QThread replyThread;
    auto *barrier = new QObject;
    barrier->moveToThread(&replyThread);
    QObject::connect(&replyThread, &QThread::finished, barrier,
                     &QObject::deleteLater);
    replyThread.start();

    auto abortCount = std::make_shared<std::atomic<int>>(0);
    auto destructionCount = std::make_shared<std::atomic<int>>(0);
    {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .finish = false,
            .abortCount = abortCount,
            .destructionCount = destructionCount,
            .replyThread = &replyThread,
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(validSseRequest(), log.callbacks());
        EXPECT_TRUE(handle->active());

        // The first barrier follows the queued abort/deleteLater call. The
        // second follows the resulting deferred-delete event.
        EXPECT_TRUE(QMetaObject::invokeMethod(
            barrier, [] {}, Qt::BlockingQueuedConnection));
        runQtEventLoop();
        EXPECT_TRUE(QMetaObject::invokeMethod(
            barrier, [] {}, Qt::BlockingQueuedConnection));

        EXPECT_EQ(abortCount->load(std::memory_order_relaxed), 1);
        EXPECT_EQ(destructionCount->load(std::memory_order_relaxed), 1);
        EXPECT_FALSE(handle->active());
        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("failure")}));
        EXPECT_TRUE(log.failure);
        if (log.failure)
        {
            EXPECT_EQ(*log.failure, TransportFailure::Network);
        }
    }

    replyThread.quit();
    replyThread.wait();
}

TEST(RumbleTransportQt, RedirectToForeignReplyKeepsQueuedFailureAlive)
{
    QThread replyThread;
    auto *barrier = new QObject;
    barrier->moveToThread(&replyThread);
    QObject::connect(&replyThread, &QThread::finished, barrier,
                     &QObject::deleteLater);
    replyThread.start();

    auto abortCount = std::make_shared<std::atomic<int>>(0);
    auto destructionCount = std::make_shared<std::atomic<int>>(0);
    {
        const auto request = validSseRequest();
        FakeNetworkAccessManager manager({
            {
                .status = 302,
                .redirect = request.url,
            },
            {
                .status = 200,
                .finish = false,
                .abortCount = abortCount,
                .destructionCount = destructionCount,
                .replyThread = &replyThread,
            },
        });
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(request, log.callbacks());

        const auto initialReplies = manager.findChildren<QNetworkReply *>();
        EXPECT_EQ(initialReplies.size(), 1);
        if (!initialReplies.isEmpty())
        {
            auto *initialReply =
                static_cast<FakeNetworkReply *>(initialReplies.front());
            initialReply->deliver();
            QCoreApplication::sendPostedEvents(initialReply,
                                               QEvent::DeferredDelete);
        }

        // The detached redirect reply has now been destroyed before the
        // queued terminal callback. Its old generation must not retire the
        // operation created by the failed replacement attempt.
        EXPECT_TRUE(handle->active());
        EXPECT_EQ(manager.requests().size(), 2U);
        EXPECT_TRUE(QMetaObject::invokeMethod(
            barrier, [] {}, Qt::BlockingQueuedConnection));
        runQtEventLoop();
        EXPECT_TRUE(QMetaObject::invokeMethod(
            barrier, [] {}, Qt::BlockingQueuedConnection));

        EXPECT_EQ(abortCount->load(std::memory_order_relaxed), 1);
        EXPECT_EQ(destructionCount->load(std::memory_order_relaxed), 1);
        EXPECT_FALSE(handle->active());
        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("failure")}));
        EXPECT_TRUE(log.failure);
        if (log.failure)
        {
            EXPECT_EQ(*log.failure, TransportFailure::Network);
        }
    }

    replyThread.quit();
    replyThread.wait();
}

TEST(RumbleTransportQt, RejectsRedirectIdentityChangesAndExcessHops)
{
    const auto expectRejected = [](TransportRequest request,
                                   const QUrl &target) {
        FakeNetworkAccessManager manager({{
            .status = 302,
            .redirect = target,
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(std::move(request), log.callbacks());
        runQtEventLoop();
        EXPECT_EQ(manager.requests().size(), 1U);
        EXPECT_EQ(log.heads, 0);
        EXPECT_EQ(log.completions, 0);
        EXPECT_EQ(log.failures, 1);
        ASSERT_TRUE(log.failure);
        EXPECT_EQ(*log.failure, TransportFailure::RedirectRejected);
    };

    expectRejected(validSseRequest(),
                   QUrl(QStringLiteral(
                       "https://web7.rumble.com/chat/api/chat/2002/stream")));
    expectRejected(validEmbedRequest(), embedUrl(QStringLiteral("vother")));
    expectRejected(validHtmlRequest(),
                   QUrl(QStringLiteral("https://rumble.com/c/other/live/")));
    expectRejected(
        validHtmlRequest(),
        QUrl(QStringLiteral("https://rumble.com/user/fixture-channel/live/")));
    expectRejected(
        validHtmlRequest(),
        QUrl(QStringLiteral("https://rumble.com/vother-title.html")));

    auto request = validSseRequest();
    request.maxRedirects = 1;
    const auto sameUrl = request.url;
    FakeNetworkAccessManager manager({
        {.status = 302, .redirect = sameUrl},
        {.status = 302, .redirect = sameUrl},
    });
    RumbleQtTransport transport(manager, nullptr);
    QtCallbackLog log;
    auto handle = transport.start(std::move(request), log.callbacks());
    runQtEventLoop();
    EXPECT_EQ(manager.requests().size(), 2U);
    EXPECT_EQ(log.heads, 0);
    EXPECT_EQ(log.completions, 0);
    EXPECT_EQ(log.failures, 1);
    ASSERT_TRUE(log.failure);
    EXPECT_EQ(*log.failure, TransportFailure::RedirectRejected);
}

TEST(RumbleTransportQt, EnforcesConcreteBodyAndHeaderBounds)
{
    {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .body = QByteArrayLiteral("12345"),
        }});
        RumbleQtTransport transport(manager, nullptr);
        auto request = validSseRequest();
        request.maxBodyBytes = 4;
        QtCallbackLog log;
        auto handle = transport.start(std::move(request), log.callbacks());
        runQtEventLoop();
        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("head"),
                                                   QStringLiteral("body"),
                                                   QStringLiteral("failure")}));
        EXPECT_EQ(log.body, QByteArrayLiteral("1234"));
        EXPECT_EQ(log.failures, 1);
        ASSERT_TRUE(log.failure);
        EXPECT_EQ(*log.failure, TransportFailure::BodyLimit);
    }

    const auto expectHeaderLimit = [](TransportRequest request,
                                      std::vector<Header> headers) {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = std::move(headers),
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(std::move(request), log.callbacks());
        runQtEventLoop();
        EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("failure")}));
        EXPECT_EQ(log.heads, 0);
        EXPECT_EQ(log.completions, 0);
        EXPECT_EQ(log.failures, 1);
        ASSERT_TRUE(log.failure);
        EXPECT_EQ(*log.failure, TransportFailure::HeaderLimit);
    };

    const auto expectAccepted = [](TransportRequest request,
                                   std::vector<Header> headers) {
        FakeNetworkAccessManager manager({{
            .status = 200,
            .headers = std::move(headers),
        }});
        RumbleQtTransport transport(manager, nullptr);
        QtCallbackLog log;
        auto handle = transport.start(std::move(request), log.callbacks());
        runQtEventLoop();
        EXPECT_EQ(log.order,
                  (std::vector<QString>{QStringLiteral("head"),
                                        QStringLiteral("complete")}));
        EXPECT_EQ(log.heads, 1);
        EXPECT_EQ(log.completions, 1);
        EXPECT_EQ(log.failures, 0);
    };

    const Header contentType = {
        QByteArrayLiteral("Content-Type"),
        QByteArrayLiteral("text/html"),
    };
    const auto wireBytes = [](const Header &header) {
        return header.name.size() + header.value.size() + 4;
    };

    std::vector<Header> exactByteBound = {contentType};
    const Header paddingTemplate = {
        QByteArrayLiteral("X-Padding"),
        {},
    };
    const auto paddingBytes = MAX_RESPONSE_HEADER_BYTES -
                              wireBytes(contentType) -
                              wireBytes(paddingTemplate);
    ASSERT_GT(paddingBytes, 16 * 1024);
    exactByteBound.push_back(
        {paddingTemplate.name, QByteArray(paddingBytes, 'x')});
    expectAccepted(validHtmlRequest(), exactByteBound);

    auto overByteBound = exactByteBound;
    overByteBound.back().value.append('x');
    expectHeaderLimit(validHtmlRequest(), std::move(overByteBound));

    std::vector<Header> exactCountBound = {contentType};
    for (int index = 1; index < MAX_RESPONSE_HEADERS; ++index)
    {
        exactCountBound.push_back(
            {QByteArrayLiteral("X-Fixture-") + QByteArray::number(index),
             QByteArrayLiteral("x")});
    }
    ASSERT_EQ(exactCountBound.size(),
              static_cast<std::size_t>(MAX_RESPONSE_HEADERS));
    expectAccepted(validHtmlRequest(), exactCountBound);

    auto overCountBound = exactCountBound;
    overCountBound.push_back(
        {QByteArrayLiteral("X-Fixture-Overflow"), QByteArrayLiteral("x")});
    expectHeaderLimit(validHtmlRequest(), std::move(overCountBound));

    expectHeaderLimit(
        validHtmlRequest(),
        {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("text/html")},
         {QByteArrayLiteral("Set-Cookie"),
          QByteArrayLiteral("first=one\n\nsecond=two")}});

    auto countBound = validSseRequest();
    countBound.maxHeaders = 1;
    expectHeaderLimit(std::move(countBound),
                      {{QByteArrayLiteral("Content-Type"),
                        QByteArrayLiteral("text/event-stream")},
                       {QByteArrayLiteral("X-Test"), QByteArrayLiteral("1")}});

    auto byteBound = validSseRequest();
    byteBound.maxHeaderBytes = 8;
    expectHeaderLimit(std::move(byteBound),
                      {{QByteArrayLiteral("Content-Type"),
                        QByteArrayLiteral("text/event-stream")}});
}

TEST(RumbleTransportQt, NormalizesQtCombinedSetCookieFields)
{
    FakeNetworkAccessManager manager({{
        .status = 200,
        .headers =
            {
                {QByteArrayLiteral("Content-Type"),
                 QByteArrayLiteral("text/html")},
                {QByteArrayLiteral("Set-Cookie"),
                 QByteArrayLiteral("first=one; Secure\n"
                                   "second=two; HttpOnly\n"
                                   "third=three; SameSite=Lax")},
            },
    }});
    RumbleQtTransport transport(manager, nullptr);
    QtCallbackLog log;
    auto handle = transport.start(validHtmlRequest(), log.callbacks());
    runQtEventLoop();

    EXPECT_EQ(log.order, (std::vector<QString>{QStringLiteral("head"),
                                               QStringLiteral("complete")}));
    EXPECT_EQ(log.failures, 0);
    ASSERT_TRUE(log.responseHead);
    ASSERT_EQ(log.responseHead->headers.size(), 4U);
    EXPECT_EQ(
        std::ranges::count_if(log.responseHead->headers,
                              [](const Header &header) {
                                  return header.name.compare(
                                             QByteArrayLiteral("Set-Cookie"),
                                             Qt::CaseInsensitive) == 0;
                              }),
        3);
    for (const auto &header : log.responseHead->headers)
    {
        EXPECT_FALSE(header.name.contains('\n'));
        EXPECT_FALSE(header.value.contains('\n'));
        EXPECT_FALSE(header.name.contains('\r'));
        EXPECT_FALSE(header.value.contains('\r'));
        EXPECT_FALSE(header.name.contains('\0'));
        EXPECT_FALSE(header.value.contains('\0'));
    }
}

TEST(RumbleTransportQt, PendingDeliveryReleasesLifetimeBytesAndHeadDeadline)
{
    FakeNetworkAccessManager manager({
        {
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .finish = false,
        },
    });
    QObject owner;
    RumbleQtTransport transport(manager, &owner);
    auto request = validSseRequest();
    request.maxBodyBytes = 4;
    request.maxPendingBodyChunks = 1;
    request.timeoutMs = 1;
    request.deadlineScope = DeadlineScope::UntilFinalHead;
    request.bodyLimitScope = BodyLimitScope::PendingDelivery;
    QtCallbackLog log;
    auto handle = transport.start(request, log.callbacks());
    runQtEventLoop(10);
    ASSERT_TRUE(manager.lastReply());
    EXPECT_TRUE(handle->active());
    EXPECT_EQ(log.failures, 0);

    manager.lastReply()->deliverChunk(QByteArrayLiteral("abcd"));
    runQtEventLoop();
    EXPECT_EQ(log.body, QByteArrayLiteral("abcd"));
    EXPECT_EQ(log.failures, 0);

    // The first delivered four bytes no longer count against the pending
    // limit, so another full-limit chunk is valid on the same long response.
    manager.lastReply()->deliverChunk(QByteArrayLiteral("wxyz"));
    runQtEventLoop();
    EXPECT_EQ(log.body, QByteArrayLiteral("abcdwxyz"));
    EXPECT_EQ(log.failures, 0);
    manager.lastReply()->finishNow();
    runQtEventLoop();
    EXPECT_EQ(log.completions, 1);
}

TEST(RumbleTransportQt,
     BackpressuresPendingChunksAndSuppressesCancelledDelivery)
{
    auto makeRequest = [] {
        auto request = validSseRequest();
        request.maxBodyBytes = 128;
        request.maxPendingBodyChunks = 1;
        request.deadlineScope = DeadlineScope::UntilFinalHead;
        request.bodyLimitScope = BodyLimitScope::PendingDelivery;
        return request;
    };

    {
        FakeNetworkAccessManager manager({
            {
                .status = 200,
                .headers = {{QByteArrayLiteral("Content-Type"),
                             QByteArrayLiteral("text/event-stream")}},
                .finish = false,
            },
        });
        QObject owner;
        RumbleQtTransport transport(manager, &owner);
        QtCallbackLog log;
        auto handle = transport.start(makeRequest(), log.callbacks());
        runQtEventLoop();
        ASSERT_TRUE(manager.lastReply());
        for (int index = 0; index < 65; ++index)
            manager.lastReply()->deliverChunk(QByteArrayLiteral("x"));
        runQtEventLoop();
        EXPECT_EQ(log.body, QByteArray(65, 'x'));
        EXPECT_EQ(log.failures, 0);
        EXPECT_TRUE(handle->active());
    }

    {
        FakeNetworkAccessManager manager({
            {
                .status = 200,
                .headers = {{QByteArrayLiteral("Content-Type"),
                             QByteArrayLiteral("text/event-stream")}},
                .finish = false,
            },
        });
        QObject owner;
        RumbleQtTransport transport(manager, &owner);
        auto request = makeRequest();
        request.maxBodyBytes = 4;
        QtCallbackLog log;
        auto handle = transport.start(request, log.callbacks());
        runQtEventLoop();
        ASSERT_TRUE(manager.lastReply());
        manager.lastReply()->deliverChunk(QByteArrayLiteral("abc"));
        manager.lastReply()->deliverChunk(QByteArrayLiteral("de"));
        runQtEventLoop();
        EXPECT_EQ(log.body, QByteArrayLiteral("abcde"));
        EXPECT_EQ(log.failures, 0);
        EXPECT_TRUE(handle->active());
    }

    {
        FakeNetworkAccessManager manager({
            {
                .status = 200,
                .headers = {{QByteArrayLiteral("Content-Type"),
                             QByteArrayLiteral("text/event-stream")}},
                .finish = false,
            },
        });
        QObject owner;
        RumbleQtTransport transport(manager, &owner);
        QtCallbackLog log;
        auto handle = transport.start(makeRequest(), log.callbacks());
        runQtEventLoop();
        ASSERT_TRUE(manager.lastReply());
        manager.lastReply()->deliverChunk(QByteArrayLiteral("queued"));
        handle->cancel();
        runQtEventLoop();
        EXPECT_TRUE(log.body.isEmpty());
        EXPECT_EQ(log.completions, 0);
        EXPECT_EQ(log.failures, 0);
    }
}

TEST(RumbleTransportQt, DrainsSseBurstLargerThanPendingWindow)
{
    QByteArray body;
    const auto appendFrame = [&body](const QByteArray &type) {
        body.append(QByteArrayLiteral("data: {\"type\":\""));
        body.append(type);
        body.append(
            QByteArrayLiteral("\",\"data\":{\"users\":[],\"channels\":[],"
                              "\"config\":{},\"messages\":[],\"padding\":\""));
        body.append(QByteArray(40 * 1024, 'x'));
        body.append(QByteArrayLiteral("\"}}\n\n"));
    };
    appendFrame(QByteArrayLiteral("init"));
    for (int index = 0; index < 64; ++index)
    {
        appendFrame(QByteArrayLiteral("messages"));
    }
    ASSERT_GT(body.size(), 2 * 1024 * 1024);

    FakeNetworkAccessManager manager({
        {
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .body = std::move(body),
        },
    });
    QObject owner;
    RumbleQtTransport transport(manager, &owner);
    RumbleApi api(transport, [&owner](std::function<void()> callback) {
        QTimer::singleShot(0, &owner, std::move(callback));
    });
    std::size_t eventCount = 0;
    std::optional<StreamTerminal> terminal;
    auto handle = api.stream(QStringLiteral("1001"),
                             {
                                 .onEvents =
                                     [&](StreamBatch batch) {
                                         eventCount += batch.events.size();
                                     },
                                 .onTerminal =
                                     [&](StreamTerminal value) {
                                         terminal = std::move(value);
                                     },
                             });

    runQtEventLoop(1000);
    EXPECT_EQ(eventCount, 65U);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->outcome, Outcome::TransportError);
    ASSERT_TRUE(terminal->error);
    EXPECT_EQ(terminal->error->code, QStringLiteral("stream_eof"));
}

TEST(RumbleTransportQt, InvalidStartIsDeferredAndManagerTeardownIsSafe)
{
    RumbleQtTransport transport(nullptr);
    auto invalid = validSseRequest();
    invalid.url.setHost(QStringLiteral("example.invalid"));

    int failures = 0;
    auto handle = transport.start(
        invalid, {
                     .onFailure =
                         [&](TransportFailure failure) {
                             ++failures;
                             EXPECT_EQ(failure,
                                       TransportFailure::RedirectRejected);
                         },
                 });
    EXPECT_EQ(failures, 0);
    QCoreApplication::processEvents();
    EXPECT_EQ(failures, 1);

    auto manager = std::make_unique<QNetworkAccessManager>();
    auto injected = std::make_unique<RumbleQtTransport>(*manager, nullptr);
    manager.reset();

    failures = 0;
    auto afterDestruction = injected->start(
        validSseRequest(), {
                               .onFailure =
                                   [&](TransportFailure failure) {
                                       ++failures;
                                       EXPECT_EQ(failure,
                                                 TransportFailure::Network);
                                   },
                           });
    EXPECT_EQ(failures, 0);
    QCoreApplication::processEvents();
    EXPECT_EQ(failures, 1);
}

TEST(RumbleTransportQt, ActiveManagerTeardownRetiresDestroyedReply)
{
    auto abortCount = std::make_shared<std::atomic<int>>(0);
    auto destructionCount = std::make_shared<std::atomic<int>>(0);
    auto manager =
        std::make_unique<FakeNetworkAccessManager>(std::vector<QtReplyScript>{{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .finish = false,
            .abortCount = abortCount,
            .destructionCount = destructionCount,
        }});
    RumbleQtTransport transport(*manager, nullptr);
    QtCallbackLog log;
    auto handle = transport.start(validSseRequest(), log.callbacks());
    ASSERT_TRUE(handle->active());
    EXPECT_EQ(manager->findChildren<QNetworkReply *>().size(), 1);

    // QNetworkAccessManager synchronously destroys its active replies. The
    // reply destruction hook must retire the operation without trying to call
    // abort() on an object whose manager is tearing down.
    manager.reset();
    EXPECT_EQ(abortCount->load(std::memory_order_relaxed), 0);
    EXPECT_EQ(destructionCount->load(std::memory_order_relaxed), 1);
    EXPECT_FALSE(handle->active());

    runQtEventLoop();
    EXPECT_TRUE(log.order.empty());
    EXPECT_EQ(log.heads, 0);
    EXPECT_EQ(log.completions, 0);
    EXPECT_EQ(log.failures, 0);
}

TEST(RumbleTransportQt, ManagerOwnedTransportRetiresReplyBeforeTeardown)
{
    auto abortCount = std::make_shared<std::atomic<int>>(0);
    auto destructionCount = std::make_shared<std::atomic<int>>(0);
    auto manager =
        std::make_unique<FakeNetworkAccessManager>(std::vector<QtReplyScript>{{
            .status = 200,
            .headers = {{QByteArrayLiteral("Content-Type"),
                         QByteArrayLiteral("text/event-stream")}},
            .finish = false,
            .abortCount = abortCount,
            .destructionCount = destructionCount,
        }});
    auto *transport = new RumbleQtTransport(*manager, manager.get());
    QtCallbackLog log;
    auto handle = transport->start(validSseRequest(), log.callbacks());
    ASSERT_TRUE(handle->active());
    EXPECT_EQ(manager->findChildren<QNetworkReply *>().size(), 1);

    // The manager deletes active replies before QObject tears down its child
    // transport. Reply destruction therefore retires the operation first.
    manager.reset();
    EXPECT_EQ(abortCount->load(std::memory_order_relaxed), 0);
    EXPECT_EQ(destructionCount->load(std::memory_order_relaxed), 1);
    EXPECT_FALSE(handle->active());

    runQtEventLoop();
    EXPECT_TRUE(log.order.empty());
    EXPECT_EQ(log.heads, 0);
    EXPECT_EQ(log.completions, 0);
    EXPECT_EQ(log.failures, 0);
}

TEST(RumbleTransportQt, OwnerDestructionAbortsWhileManagerLives)
{
    auto abortCount = std::make_shared<std::atomic<int>>(0);
    auto destructionCount = std::make_shared<std::atomic<int>>(0);
    FakeNetworkAccessManager manager({{
        .status = 200,
        .headers = {{QByteArrayLiteral("Content-Type"),
                     QByteArrayLiteral("text/event-stream")}},
        .finish = false,
        .abortCount = abortCount,
        .destructionCount = destructionCount,
    }});
    auto *owner = new QObject;
    auto *transport = new RumbleQtTransport(manager, owner);
    QtCallbackLog log;
    auto handle = transport->start(validSseRequest(), log.callbacks());
    ASSERT_TRUE(handle->active());
    const auto replies = manager.findChildren<QNetworkReply *>();
    ASSERT_EQ(replies.size(), 1);
    QPointer<QNetworkReply> reply = replies.front();

    delete owner;
    EXPECT_EQ(abortCount->load(std::memory_order_relaxed), 1);
    EXPECT_FALSE(handle->active());

    // deleteLater() is part of the cancellation contract, but a nested test
    // event loop does not guarantee delivery of deferred deletes that were
    // posted before it started. Flush this reply's event explicitly and keep
    // the destruction assertion as proof that owner teardown scheduled it.
    QCoreApplication::sendPostedEvents(reply.data(), QEvent::DeferredDelete);
    EXPECT_TRUE(reply.isNull());
    EXPECT_EQ(destructionCount->load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(log.order.empty());
    EXPECT_EQ(log.heads, 0);
    EXPECT_EQ(log.completions, 0);
    EXPECT_EQ(log.failures, 0);
}

TEST(RumbleTransportQt, OwnerDestructionSuppressesQueuedCallbacks)
{
    auto *owner = new QObject;
    auto *transport = new RumbleQtTransport(owner);
    auto invalid = validSseRequest();
    invalid.url.setHost(QStringLiteral("example.invalid"));

    int callbacks = 0;
    auto handle = transport->start(invalid, {
                                                .onFailure =
                                                    [&](TransportFailure) {
                                                        ++callbacks;
                                                    },
                                            });
    delete owner;
    QCoreApplication::processEvents();
    EXPECT_EQ(callbacks, 0);
    handle.reset();
}

TEST(RumbleApiRedaction, ErrorsContainOnlyStableCodes)
{
    const QString locatorCanary =
        QStringLiteral("locator-canary-secret.example.invalid");
    ImmediateTransport unused({});
    DeferredQueue queue;
    RumbleApi api(unused, queue.dispatcher());

    std::optional<ResolveResult> unsupported;
    auto first = api.resolve(locatorCanary, [&](ResolveResult value) {
        unsupported = std::move(value);
    });
    queue.runAll();
    ASSERT_TRUE(unsupported);
    ASSERT_TRUE(unsupported->error);
    EXPECT_FALSE(unsupported->error->code.contains(locatorCanary));
    EXPECT_EQ(unsupported->error->code, QStringLiteral("unsupported_locator"));

    ImmediateTransport failing({
        {
            .response =
                [] {
                    auto value =
                        head(503, QByteArrayLiteral("application/json"));
                    value.headers.push_back(
                        {QByteArrayLiteral("X-Canary"),
                         QByteArrayLiteral("header-canary-secret")});
                    value.headers.push_back(
                        {QByteArrayLiteral("Retry-After"),
                         QByteArrayLiteral("retry-canary-secret")});
                    return value;
                }(),
            .body = QByteArrayLiteral(
                R"({"error":"body-canary-secret","id":"id-canary-secret"})"),
        },
    });
    DeferredQueue secondQueue;
    RumbleApi failingApi(failing, secondQueue.dispatcher());
    std::optional<ResolveResult> serverError;
    auto second = failingApi.resolve(QStringLiteral("vfixture"),
                                     [&](ResolveResult value) {
                                         serverError = std::move(value);
                                     });
    secondQueue.runAll();

    ASSERT_TRUE(serverError);
    ASSERT_TRUE(serverError->error);
    EXPECT_EQ(serverError->error->code, QStringLiteral("http_failure"));
    EXPECT_FALSE(serverError->error->code.contains(QStringLiteral("canary")));
    EXPECT_FALSE(serverError->error->retry.after);

    ThrowingTransport throwing;
    DeferredQueue thirdQueue;
    RumbleApi throwingApi(throwing, thirdQueue.dispatcher());
    std::optional<ResolveResult> exceptionResult;
    auto third = throwingApi.resolve(QStringLiteral("vfixture"),
                                     [&](ResolveResult value) {
                                         exceptionResult = std::move(value);
                                     });
    thirdQueue.runAll();
    ASSERT_TRUE(exceptionResult);
    ASSERT_TRUE(exceptionResult->error);
    EXPECT_EQ(exceptionResult->error->code,
              QStringLiteral("transport_start_failure"));
    EXPECT_FALSE(exceptionResult->error->code.contains(
        QStringLiteral("exception-canary-secret")));
}
