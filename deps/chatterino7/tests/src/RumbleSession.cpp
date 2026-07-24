// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleSession.hpp"

#include "providers/rumble/RumbleBrowserLogin.hpp"
#include "providers/rumble/RumbleQtAuthTransport.hpp"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace chatterino::rumble;

namespace {
class FakeHandle final : public AuthHandle
{
public:
    explicit FakeHandle(bool &cancelled)
        : cancelled_(cancelled)
    {
    }
    ~FakeHandle() override = default;
    void cancel() noexcept override
    {
        cancelled_ = true;
    }

private:
    bool &cancelled_;
};

class FakeTransport final : public AuthTransport
{
public:
    std::unique_ptr<AuthHandle> start(AuthOperation operation, QString stream,
                                      QString text, QByteArray bearer,
                                      QByteArray requestId,
                                      AuthCallbacks callbacks) override
    {
        ++starts;
        lastOperation = operation;
        lastStream = std::move(stream);
        lastText = std::move(text);
        lastBearer = std::move(bearer);
        lastRequestId = std::move(requestId);
        pending = std::move(callbacks);
        cancelled = false;
        return std::make_unique<FakeHandle>(cancelled);
    }
    void complete(int status, QByteArray body,
                  QByteArray type = "application/json",
                  std::optional<QByteArray> retry = std::nullopt)
    {
        auto callback = std::move(pending.complete);
        pending = {};
        callback({status, std::move(type), std::move(body), std::move(retry)});
    }
    void fail(AuthFailure failure)
    {
        auto callback = std::move(pending.failed);
        pending = {};
        callback(failure);
    }
    int starts = 0;
    bool cancelled = false;
    AuthOperation lastOperation = AuthOperation::Probe;
    QString lastStream;
    QString lastText;
    QByteArray lastBearer;
    QByteArray lastRequestId;
    AuthCallbacks pending;
};

class SynchronousHostileTransport final : public AuthTransport
{
public:
    std::unique_ptr<AuthHandle> start(AuthOperation operation, QString, QString,
                                      QByteArray, QByteArray requestId,
                                      AuthCallbacks callbacks) override
    {
        ++starts;
        lastRequestId = std::move(requestId);
        if (operation == AuthOperation::Probe)
            callbacks.complete(
                {200, "application/json", R"({"user":{"id":"sync"}})", {}});
        else
            callbacks.complete({200,
                                "application/json",
                                R"({"data":{"id":"sync-message"}})",
                                {}});
        // A broken transport must not be able to complete an operation twice.
        if (callbacks.failed)
            callbacks.failed(AuthFailure::Network);
        return std::make_unique<FakeHandle>(cancelled);
    }
    int starts = 0;
    bool cancelled = false;
    QByteArray lastRequestId;
};

class AuthReply final : public QNetworkReply
{
public:
    AuthReply(const QNetworkRequest &request, int status, QByteArray body,
              QByteArray contentType, std::optional<QUrl> redirect,
              QObject *parent)
        : QNetworkReply(parent)
        , body_(std::move(body))
        , redirect_(std::move(redirect))
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, std::move(contentType));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (redirect_)
                setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                             *redirect_);
            setFinished(true);
            Q_EMIT finished();
        });
    }
    void abort() override
    {
    }
    bool isSequential() const override
    {
        return true;
    }
    qint64 bytesAvailable() const override
    {
        return body_.size() + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const auto count = std::min<qint64>(maximum, body_.size());
        if (count <= 0)
            return -1;
        std::memcpy(data, body_.constData(), static_cast<std::size_t>(count));
        body_.remove(0, count);
        return count;
    }

private:
    QByteArray body_;
    std::optional<QUrl> redirect_;
};

class CaptureAuthManager final : public QNetworkAccessManager
{
public:
    int status = 200;
    QByteArray response = R"({"data":{"id":"ok"}})";
    QByteArray contentType = "application/json";
    std::optional<QUrl> redirect;
    Operation operation = GetOperation;
    QNetworkRequest request;
    QByteArray body;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &req,
                                 QIODevice *outgoing) override
    {
        operation = op;
        request = req;
        body = outgoing ? outgoing->readAll() : QByteArray{};
        return new AuthReply(req, status, response, contentType, redirect,
                             this);
    }
};

void drainEventsUntil(const std::function<bool()> &done)
{
    for (int i = 0; i < 20 && !done(); ++i)
        QCoreApplication::processEvents();
}

void validate(FakeTransport &transport, SessionController &session,
              const QByteArray &secret = "SYNTHETIC_SESSION_CANARY")
{
    ASSERT_TRUE(session.importSession(secret));
    bool done = false;
    session.validate([&](bool ok, QString) {
        EXPECT_TRUE(ok);
        done = true;
    });
    EXPECT_EQ(transport.lastOperation, AuthOperation::Probe);
    transport.complete(200, R"({"user":{"id":"SYNTHETIC_USER"}})");
    EXPECT_TRUE(done);
    EXPECT_EQ(session.state(), SessionState::Valid);
}
}  // namespace

TEST(RumbleBrowserLogin, ParsesOnlyBoundedClosedVocabularyProtocol)
{
    auto success = parseBrowserLoginOutput(
        QByteArrayLiteral("RUMBLE_LOGIN_V1 OK U1lOVEhFVElDX1NFU1NJT04=\n"));
    EXPECT_EQ(success.outcome, BrowserLoginOutcome::Session);
    EXPECT_EQ(success.session, QByteArrayLiteral("SYNTHETIC_SESSION"));
    EXPECT_EQ(success.userMessage,
              QStringLiteral("Rumble sign-in completed; confirming the account…"));

    const std::vector<QByteArray> rejected = {
        QByteArrayLiteral("SYNTHETIC_SESSION"),
        QByteArrayLiteral("RUMBLE_LOGIN_V1 OK raw;cookie\n"),
        QByteArrayLiteral("RUMBLE_LOGIN_V1 OK !!!\n"),
        QByteArrayLiteral("RUMBLE_LOGIN_V1 OK U1lOVEhFVElD\nextra\n"),
        QByteArray(8 * 1024 + 1, 'x'),
    };
    for (const auto &value : rejected)
    {
        SCOPED_TRACE(value.left(80).constData());
        EXPECT_EQ(parseBrowserLoginOutput(value).outcome,
                  BrowserLoginOutcome::Failed);
    }

    const auto cancelled = parseBrowserLoginOutput(
        QByteArrayLiteral("RUMBLE_LOGIN_V1 CANCELLED\n"));
    EXPECT_EQ(cancelled.outcome, BrowserLoginOutcome::Cancelled);
    EXPECT_EQ(cancelled.userMessage,
              QStringLiteral("Rumble sign-in was cancelled."));

    const auto timedOut = parseBrowserLoginOutput(
        QByteArrayLiteral("RUMBLE_LOGIN_V1 TIMEOUT\n"));
    EXPECT_EQ(timedOut.outcome, BrowserLoginOutcome::Failed);
    EXPECT_EQ(timedOut.userMessage,
              QStringLiteral("Rumble sign-in took too long. Try again."));

    const auto browserStartFailed = parseBrowserLoginOutput(
        QByteArrayLiteral("RUMBLE_LOGIN_V1 BROWSER_START_FAILED\n"));
    EXPECT_EQ(browserStartFailed.outcome, BrowserLoginOutcome::Unavailable);
    EXPECT_EQ(browserStartFailed.userMessage,
              QStringLiteral("Could not open the Rumble sign-in window. Try "
                             "again."));
}

TEST(RumbleBrowserLogin, ParsesOnlyBoundedBrowserAccountName)
{
    EXPECT_EQ(parseBrowserAccountName(
                  QByteArrayLiteral(R"({"value":"Synthetic User"})")),
              QStringLiteral("Synthetic User"));
    for (const auto &value : {
             QByteArrayLiteral(R"({"value":null})"),
             QByteArrayLiteral(R"({"value":""})"),
             QByteArrayLiteral(R"({"value":42})"),
             QByteArrayLiteral(R"({"value":"bad\u000auser"})"),
             QByteArrayLiteral(R"({"value":{"username":"Synthetic"}})"),
             QByteArrayLiteral("not-json"),
         })
    {
        EXPECT_FALSE(parseBrowserAccountName(value));
    }
    EXPECT_FALSE(parseBrowserAccountName(QByteArrayLiteral(R"({"value":")") +
                                         QByteArray(257, 'x') +
                                         QByteArrayLiteral(R"("})")));
}

TEST(RumbleBrowserLogin, ParsesOnlyBoundedKetchConsentState)
{
    EXPECT_EQ(
        parseBrowserConsentState(QByteArrayLiteral(R"({"value":"pending"})")),
        BrowserConsentState::Pending);
    EXPECT_EQ(
        parseBrowserConsentState(QByteArrayLiteral(R"({"value":"resolved"})")),
        BrowserConsentState::Resolved);
    EXPECT_EQ(
        parseBrowserConsentState(QByteArrayLiteral(R"({"value":"rejected"})")),
        BrowserConsentState::Rejected);
    EXPECT_EQ(
        parseBrowserConsentState(QByteArrayLiteral(R"({"value":"rumble"})")),
        BrowserConsentState::RumblePage);
    EXPECT_EQ(
        parseBrowserConsentState(QByteArrayLiteral(R"({"value":"external"})")),
        BrowserConsentState::ExternalPage);

    for (const auto &value : {
             QByteArrayLiteral(R"({"value":"unknown"})"),
             QByteArrayLiteral(R"({"value":true})"),
             QByteArrayLiteral(R"({"value":{"marketing":false}})"),
             QByteArrayLiteral(R"({"status":"resolved"})"),
             QByteArrayLiteral("not-json"),
         })
    {
        EXPECT_FALSE(parseBrowserConsentState(value));
    }
    EXPECT_FALSE(parseBrowserConsentState(QByteArray(256 * 1024 + 1, 'x')));
}

TEST(RumbleBrowserLogin, ConsentGuardLeavesOnlyKetchInteractive)
{
    const auto script = browserConsentMonitorScript();
    EXPECT_TRUE(script.contains(QStringLiteral("document.body.children")));
    EXPECT_TRUE(
        script.contains(QStringLiteral("element.id === 'lanyard_root'")));
    EXPECT_TRUE(
        script.contains(QStringLiteral("element.id === 'lanyard_fab_button'")));
    EXPECT_TRUE(script.contains(QStringLiteral("setAttribute('inert', '')")));
    EXPECT_TRUE(script.contains(
        QStringLiteral("style.setProperty('pointer-events', 'none'")));
    EXPECT_TRUE(script.contains(QStringLiteral("window.ketch('getConsent'")));
    EXPECT_TRUE(script.contains(QStringLiteral(
        "window.ketch('on', 'hideExperience', state.readConsent)")));
    EXPECT_TRUE(script.contains(
        QStringLiteral("purposes.essential_services === true")));
    EXPECT_TRUE(script.contains(QStringLiteral("state.reject()")));
    EXPECT_TRUE(script.contains(QStringLiteral("state.enableRumble()")));
    EXPECT_FALSE(script.contains(QStringLiteral("textContent")));
    EXPECT_FALSE(script.contains(QStringLiteral(".click()")));
    EXPECT_FALSE(
        script.contains(QStringLiteral("window.ketch('on', 'consent'")));
    EXPECT_FALSE(script.contains(QStringLiteral("showPreferenceExperience")));
}

TEST(RumbleBrowserLogin, ParsesOnlySuccessfulBoundedSeleniumManagerJson)
{
    const auto valid = parseSeleniumManagerOutput(QByteArrayLiteral(R"({
        "logs": [],
        "result": {
            "code": 0,
            "driver_path": "/synthetic/chromedriver",
            "browser_path": "/synthetic/chromium"
        }
    })"));
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->driverPath, QStringLiteral("/synthetic/chromedriver"));
    EXPECT_EQ(valid->browserPath, QStringLiteral("/synthetic/chromium"));

    const std::vector<QByteArray> rejected = {
        QByteArrayLiteral("{}"),
        QByteArrayLiteral(R"({"result":{"code":1,"driver_path":"/driver"}})"),
        QByteArrayLiteral(R"({"result":{"code":0,"driver_path":""}})"),
        QByteArrayLiteral("not json"),
        QByteArray(256 * 1024 + 1, 'x'),
    };
    for (const auto &value : rejected)
    {
        EXPECT_FALSE(parseSeleniumManagerOutput(value));
    }
}

TEST(RumbleBrowserLogin, LaunchesPortraitBrowserBeforeAttachingDriver)
{
    const auto geometry =
        fitPortraitBrowserWindow(QRect(0, 0, 1920, 1080), 1.0);
    EXPECT_EQ(geometry.x, 585);
    EXPECT_EQ(geometry.y, 40);
    EXPECT_EQ(geometry.width, 750);
    EXPECT_EQ(geometry.height, 1000);

    const auto shortDisplay =
        fitPortraitBrowserWindow(QRect(0, 0, 1366, 768), 1.0);
    EXPECT_GE(shortDisplay.x, 0);
    EXPECT_GE(shortDisplay.y, 0);
    EXPECT_LE(shortDisplay.x + shortDisplay.width, 1366);
    EXPECT_LE(shortDisplay.y + shortDisplay.height, 768);
    EXPECT_LT(shortDisplay.height, 1000);

    const auto smallDisplay =
        fitPortraitBrowserWindow(QRect(24, 48, 640, 480), 1.0);
    EXPECT_EQ(smallDisplay.x, 188);
    EXPECT_EQ(smallDisplay.y, 80);
    EXPECT_EQ(smallDisplay.width, 312);
    EXPECT_EQ(smallDisplay.height, 416);
    EXPECT_GE(smallDisplay.x, 24);
    EXPECT_GE(smallDisplay.y, 48);
    EXPECT_LE(smallDisplay.x + smallDisplay.width, 664);
    EXPECT_LE(smallDisplay.y + smallDisplay.height, 528);

    const auto arguments = chromiumBrowserArguments(
        QStringLiteral("/synthetic/profile"), 41234, geometry, true);
    EXPECT_TRUE(arguments.contains(QStringLiteral("--ozone-platform=x11")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("--disable-gpu")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--kiosk")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--start-fullscreen")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--start-maximized")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--force-app-mode")));
    EXPECT_TRUE(arguments.contains(
        QStringLiteral("--app=https://rumble.com/account/login")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("--window-size=750,1000")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("--window-position=585,40")));
    EXPECT_TRUE(arguments.contains(
        QStringLiteral("--remote-debugging-address=127.0.0.1")));
    EXPECT_TRUE(
        arguments.contains(QStringLiteral("--remote-debugging-port=41234")));
    EXPECT_TRUE(arguments.contains(
        QStringLiteral("--user-data-dir=/synthetic/profile")));
    for (const auto &argument : arguments)
    {
        EXPECT_FALSE(argument.contains(QStringLiteral("enable-automation")));
        EXPECT_FALSE(argument.contains(QStringLiteral("AutomationControlled")));
        EXPECT_FALSE(argument.contains(QStringLiteral("test-type=webdriver")));
        EXPECT_FALSE(argument.contains(QStringLiteral("user-agent")));
    }

    const auto nativeArguments = chromiumBrowserArguments(
        QStringLiteral("/synthetic/profile"), 41234, geometry, false);
    EXPECT_FALSE(
        nativeArguments.contains(QStringLiteral("--ozone-platform=x11")));
    EXPECT_FALSE(nativeArguments.contains(QStringLiteral("--disable-gpu")));

    const auto firefoxArguments = firefoxBrowserArguments(geometry);
    EXPECT_TRUE(firefoxArguments.contains(QStringLiteral("--width=750")));
    EXPECT_TRUE(firefoxArguments.contains(QStringLiteral("--height=1000")));
    EXPECT_FALSE(firefoxArguments.contains(QStringLiteral("--kiosk")));
    EXPECT_FALSE(firefoxArguments.contains(QStringLiteral("--fullscreen")));

    const auto options = chromiumAttachOptions(41234);
    EXPECT_EQ(options.size(), 1);
    EXPECT_EQ(options.value(QStringLiteral("debuggerAddress")).toString(),
              QStringLiteral("127.0.0.1:41234"));
}

TEST(RumbleBrowserLogin,
     UsesLogicalSecondaryScreenCoordinatesWithoutDoubleScaling)
{
    const auto geometry =
        fitPortraitBrowserWindow(QRect(-1280, 72, 1280, 720), 2.0);
    EXPECT_EQ(geometry.x, -1655);
    EXPECT_EQ(geometry.y, 364);
    EXPECT_EQ(geometry.width, 750);
    EXPECT_EQ(geometry.height, 1000);

    const auto arguments = chromiumBrowserArguments(
        QStringLiteral("/synthetic/profile"), 41234, geometry, false);
    EXPECT_TRUE(
        arguments.contains(QStringLiteral("--window-position=-1655,364")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("--window-size=750,1000")));
}

TEST(RumbleAccount, ImportValidateAndClearAreMemoryOnlyAndGenerationGuarded)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);
    const auto validGeneration = session.generation();
    EXPECT_EQ(session.accountId(), QStringLiteral("SYNTHETIC_USER"));
    ASSERT_TRUE(session.identity());
    EXPECT_EQ(session.identity()->userID, QStringLiteral("SYNTHETIC_USER"));
    bool staleCallback = false;
    session.send(QStringLiteral("42"), QStringLiteral("pending"),
                 [&](SendResult) {
                     staleCallback = true;
                 });
    session.clear();
    EXPECT_GT(session.generation(), validGeneration);
    EXPECT_EQ(session.state(), SessionState::Empty);
    EXPECT_FALSE(session.isWritable(QStringLiteral("42")));
    EXPECT_TRUE(transport.cancelled);
    EXPECT_FALSE(staleCallback);
    EXPECT_FALSE(session.importSession("bad;cookie"));
}

TEST(RumbleAccount, ProbeIdentityRequiresLosslessPositiveNumericId)
{
    FakeTransport transport;
    SessionController session(transport);

    ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
    session.validate({});
    transport.complete(200,
                       R"({"user":{"id":42,"username":"Synthetic User"}})");
    ASSERT_TRUE(session.identity());
    EXPECT_EQ(session.identity()->userID, QStringLiteral("42"));
    EXPECT_EQ(session.identity()->username, QStringLiteral("Synthetic User"));

    for (const auto body : {
             R"({"user":{"id":0}})",
             R"({"user":{"id":-1}})",
             R"({"user":{"id":1.5}})",
         })
    {
        ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
        session.validate({});
        transport.complete(200, body);
        EXPECT_EQ(session.state(), SessionState::Unvalidated);
        EXPECT_FALSE(session.identity());
    }
}

TEST(RumbleAuth, SynchronousAndDuplicateCallbacksRetireWithoutStaleBusyHandle)
{
    SynchronousHostileTransport transport;
    SessionController session(transport);
    ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
    int probeCallbacks = 0;
    session.validate([&](bool ok, QString) {
        EXPECT_TRUE(ok);
        ++probeCallbacks;
    });
    EXPECT_EQ(probeCallbacks, 1);
    EXPECT_EQ(session.state(), SessionState::Valid);

    int sendCallbacks = 0;
    session.send(QStringLiteral("7"), QStringLiteral("hello"),
                 [&](SendResult result) {
                     EXPECT_EQ(result.outcome, SendOutcome::Confirmed);
                     ++sendCallbacks;
                 });
    EXPECT_EQ(sendCallbacks, 1);
    EXPECT_EQ(transport.lastRequestId.size(), 43);
    EXPECT_TRUE(session.isWritable(QStringLiteral("7")));
}

TEST(RumbleAuth, ProbeRejectionClearsSessionAndStaleCompletionIsInert)
{
    FakeTransport transport;
    SessionController session(transport);
    ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
    bool called = false;
    session.validate([&](bool ok, QString) {
        EXPECT_FALSE(ok);
        called = true;
    });
    transport.complete(403, "{}");
    EXPECT_TRUE(called);
    EXPECT_EQ(session.state(), SessionState::Empty);
}

TEST(RumbleAuth, ProbeRateLimitUsesBoundedFallbackAndPreventsStorms)
{
    const auto now = QDateTime::fromSecsSinceEpoch(1000, Qt::UTC);
    FakeTransport transport;
    SessionController session(transport, [now] {
        return now;
    });
    ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
    session.validate({});
    transport.complete(429, "{}", "application/json", QByteArray("invalid"));
    EXPECT_EQ(session.blockedUntil(), now.addSecs(60));
    const auto before = transport.starts;
    session.validate({});
    EXPECT_EQ(transport.starts, before);
}

TEST(RumbleSend, ConfirmedSendIsOneShotAndCarriesFrozenShapeInputs)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);
    std::optional<SendResult> result;
    session.send(QStringLiteral("42"), QStringLiteral("hello"),
                 [&](SendResult value) {
                     result = std::move(value);
                 });
    EXPECT_EQ(transport.starts, 2);
    EXPECT_EQ(transport.lastOperation, AuthOperation::Send);
    EXPECT_EQ(transport.lastStream, QStringLiteral("42"));
    EXPECT_EQ(transport.lastText, QStringLiteral("hello"));
    EXPECT_EQ(transport.lastRequestId.size(), 43);
    transport.complete(200, R"({"data":{"id":"message-1"}})");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, SendOutcome::Confirmed);
    EXPECT_EQ(result->messageId, QStringLiteral("message-1"));
    EXPECT_EQ(transport.starts, 2);
}

TEST(RumbleSend, AmbiguousFailuresNeverRetry)
{
    for (const auto mode : {0, 1, 2, 3, 4})
    {
        FakeTransport transport;
        SessionController session(transport);
        validate(transport, session);
        std::optional<SendResult> result;
        session.send(QStringLiteral("7"), QStringLiteral("once"),
                     [&](SendResult value) {
                         result = std::move(value);
                     });
        if (mode == 0)
            transport.fail(AuthFailure::Timeout);
        else if (mode == 1)
            transport.complete(500, "{}");
        else if (mode == 2)
            transport.complete(200, R"({"data":{}})");
        else if (mode == 3)
            transport.complete(408, "{}");
        else
            transport.complete(200, R"({"data":{"id":"hidden"}})", "text/html");
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, SendOutcome::Ambiguous);
        EXPECT_NE(result->userMessage.indexOf(QStringLiteral("may have sent")),
                  -1);
        EXPECT_EQ(transport.starts, 2);
    }
}

TEST(RumbleSend, CancellationIsTypedAndDoesNotRetry)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);
    std::optional<SendResult> result;
    session.send(QStringLiteral("7"), QStringLiteral("cancel"),
                 [&](SendResult value) {
                     result = std::move(value);
                 });
    transport.fail(AuthFailure::Cancelled);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, SendOutcome::Cancelled);
    EXPECT_EQ(transport.starts, 2);
}

TEST(RumbleSend, EveryAttemptUsesAUniqueObservedShapeRequestId)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);
    session.send(QStringLiteral("7"), QStringLiteral("first"), {});
    const auto first = transport.lastRequestId;
    transport.complete(200, R"({"data":{"id":"one"}})");
    session.send(QStringLiteral("7"), QStringLiteral("second"), {});
    const auto second = transport.lastRequestId;
    EXPECT_EQ(first.size(), 43);
    EXPECT_EQ(second.size(), 43);
    EXPECT_NE(first, second);
    transport.complete(200, R"({"data":{"id":"two"}})");
}

TEST(RumbleSend, StatusPoliciesInvalidateOnlyDocumentedScope)
{
    const auto now = QDateTime::fromSecsSinceEpoch(1000, Qt::UTC);
    FakeTransport transport;
    SessionController session(transport, [now] {
        return now;
    });
    validate(transport, session);

    std::optional<SendResult> result;
    session.send(QStringLiteral("7"), QStringLiteral("rate"),
                 [&](SendResult value) {
                     result = std::move(value);
                 });
    transport.complete(429, "{}", "application/json", QByteArray("60"));
    EXPECT_EQ(result->outcome, SendOutcome::DefiniteFailure);
    EXPECT_EQ(session.blockedUntil(), now.addSecs(60));
    EXPECT_FALSE(session.isWritable(QStringLiteral("7")));

    session.clear();
    validate(transport, session);
    session.send(QStringLiteral("7"), QStringLiteral("forbidden"),
                 [&](SendResult value) {
                     result = std::move(value);
                 });
    transport.complete(403, "{}");
    EXPECT_EQ(session.state(), SessionState::Valid);
    EXPECT_FALSE(session.isWritable(QStringLiteral("7")));
    EXPECT_TRUE(session.isWritable(QStringLiteral("8")));

    session.send(QStringLiteral("8"), QStringLiteral("expired"),
                 [&](SendResult value) {
                     result = std::move(value);
                 });
    transport.complete(401, "{}");
    EXPECT_EQ(session.state(), SessionState::Unvalidated);
}

TEST(RumbleSend, MissingInvalidAndDateRetryAfterAreBounded)
{
    const auto now = QDateTime::fromSecsSinceEpoch(1000, Qt::UTC);
    for (const auto &retry :
         {std::optional<QByteArray>{},
          std::optional<QByteArray>{QByteArray("invalid")},
          std::optional<QByteArray>{
              now.addSecs(120).toString(Qt::RFC2822Date).toLatin1()}})
    {
        FakeTransport transport;
        SessionController session(transport, [now] {
            return now;
        });
        validate(transport, session);
        session.send(QStringLiteral("7"), QStringLiteral("rate"), {});
        transport.complete(429, "{}", "application/json", retry);
        ASSERT_TRUE(session.blockedUntil());
        const auto seconds = now.secsTo(*session.blockedUntil());
        EXPECT_GE(seconds, 0);
        EXPECT_LE(seconds, 24 * 60 * 60);
        if (retry && *retry != QByteArray("invalid"))
            EXPECT_EQ(seconds, 120);
        else
            EXPECT_EQ(seconds, 60);
    }
}

TEST(RumbleSend, AccountReplacementAndShutdownSuppressStaleMutationCompletion)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);
    bool called = false;
    session.send(QStringLiteral("7"), QStringLiteral("old draft"),
                 [&](SendResult) {
                     called = true;
                 });
    ASSERT_TRUE(session.importSession("REPLACEMENT_SESSION_CANARY"));
    EXPECT_TRUE(transport.cancelled);
    // A hostile transport may still deliver after cancellation. Generation
    // validation makes that completion inert.
    transport.complete(200, R"({"data":{"id":"stale"}})");
    EXPECT_FALSE(called);
    EXPECT_EQ(session.state(), SessionState::Unvalidated);

    session.shutdown();
    EXPECT_EQ(session.state(), SessionState::Empty);
    EXPECT_FALSE(session.importSession("AFTER_SHUTDOWN_CANARY"));
}

TEST(RumbleSend, LocalBoundsRejectBeforeMutation)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);
    const auto before = transport.starts;
    std::optional<SendResult> result;
    session.send(
        QStringLiteral("7"),
        QString(SessionController::ABSOLUTE_TEXT_LIMIT + 1, QLatin1Char('x')),
        [&](SendResult value) {
            result = std::move(value);
        });
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, SendOutcome::DefiniteFailure);
    EXPECT_EQ(transport.starts, before);
}

TEST(RumbleSecretRedaction, PublicResultsNeverContainInputs)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session, "SECRET_REDaction_CANARY");
    std::optional<SendResult> result;
    session.send(QStringLiteral("998877"), QStringLiteral("BODY_CANARY"),
                 [&](SendResult value) {
                     result = std::move(value);
                 });
    transport.fail(AuthFailure::Network);
    ASSERT_TRUE(result);
    const auto publicText = result->userMessage + session.accountId();
    EXPECT_FALSE(
        publicText.contains(QStringLiteral("SECRET_REDaction_CANARY")));
    EXPECT_FALSE(publicText.contains(QStringLiteral("BODY_CANARY")));
    EXPECT_FALSE(publicText.contains(QStringLiteral("998877")));
}

TEST(RumbleAuthTransport, BuildsOnlyFrozenPostAndDisablesCookieJarPolicy)
{
    QObject owner;
    CaptureAuthManager manager;
    RumbleQtAuthTransport transport(manager, &owner);
    bool completed = false;
    auto handle = transport.start(
        AuthOperation::Send, QStringLiteral("42"), QStringLiteral("hello"),
        QByteArray("SYNTHETIC_SESSION_CANARY"), QByteArray(43, 'r'),
        {.complete =
             [&](AuthResponse) {
                 completed = true;
             },
         .failed =
             [&](AuthFailure) {
                 FAIL();
             }});
    ASSERT_TRUE(handle);
    drainEventsUntil([&] {
        return completed;
    });
    EXPECT_TRUE(completed);
    EXPECT_EQ(manager.operation, QNetworkAccessManager::PostOperation);
    EXPECT_EQ(manager.request.url().host(), QStringLiteral("web7.rumble.com"));
    EXPECT_EQ(manager.request.url().path(),
              QStringLiteral("/chat/api/chat/42/message"));
    EXPECT_EQ(manager.request.rawHeader("Origin"), "https://rumble.com");
    EXPECT_EQ(manager.request.rawHeader("Cookie"),
              "u_s=SYNTHETIC_SESSION_CANARY");
    EXPECT_EQ(
        manager.request.attribute(QNetworkRequest::CookieLoadControlAttribute)
            .toInt(),
        static_cast<int>(QNetworkRequest::Manual));
    EXPECT_EQ(
        manager.request.attribute(QNetworkRequest::CookieSaveControlAttribute)
            .toInt(),
        static_cast<int>(QNetworkRequest::Manual));
    const auto root = QJsonDocument::fromJson(manager.body).object();
    const auto data = root.value(QStringLiteral("data")).toObject();
    EXPECT_EQ(data.value(QStringLiteral("request_id")).toString().size(), 43);
    EXPECT_EQ(data.value(QStringLiteral("message"))
                  .toObject()
                  .value(QStringLiteral("text"))
                  .toString(),
              QStringLiteral("hello"));
}

TEST(RumbleAuthTransport, BuildsExactReadOnlySessionProbe)
{
    QObject owner;
    CaptureAuthManager manager;
    manager.response = R"({"user":{"id":"probe-user"}})";
    RumbleQtAuthTransport transport(manager, &owner);
    bool completed = false;
    auto handle = transport.start(AuthOperation::Probe, {}, {},
                                  QByteArray("SYNTHETIC_SESSION_CANARY"), {},
                                  {.complete =
                                       [&](AuthResponse) {
                                           completed = true;
                                       },
                                   .failed =
                                       [&](AuthFailure) {
                                           FAIL();
                                       }});
    ASSERT_TRUE(handle);
    drainEventsUntil([&] {
        return completed;
    });
    EXPECT_TRUE(completed);
    EXPECT_EQ(manager.operation, QNetworkAccessManager::GetOperation);
    EXPECT_EQ(manager.request.url().scheme(), QStringLiteral("https"));
    EXPECT_EQ(manager.request.url().host(), QStringLiteral("rumble.com"));
    EXPECT_EQ(manager.request.url().path(), QStringLiteral("/service.php"));
    EXPECT_EQ(
        QUrlQuery(manager.request.url()).queryItemValue(QStringLiteral("name")),
        QStringLiteral("user.has_unread_notifications"));
    EXPECT_EQ(manager.request.rawHeader("Cookie"),
              "u_s=SYNTHETIC_SESSION_CANARY");
    EXPECT_EQ(manager.request.rawHeader("Accept"),
              "application/json, text/plain, */*");
    EXPECT_EQ(manager.request.rawHeader("Content-Type"),
              "application/x-www-form-urlencoded");
    EXPECT_TRUE(manager.request.rawHeader("Origin").isEmpty());
    EXPECT_TRUE(manager.body.isEmpty());
    EXPECT_EQ(
        manager.request.attribute(QNetworkRequest::CookieLoadControlAttribute)
            .toInt(),
        static_cast<int>(QNetworkRequest::Manual));
    EXPECT_EQ(
        manager.request.attribute(QNetworkRequest::CookieSaveControlAttribute)
            .toInt(),
        static_cast<int>(QNetworkRequest::Manual));
}

TEST(RumbleAuthTransport, RedirectAfterPostIsAmbiguousTransportFailure)
{
    QObject owner;
    CaptureAuthManager manager;
    manager.status = 302;
    manager.redirect = QUrl(QStringLiteral("https://evil.invalid/"));
    RumbleQtAuthTransport transport(manager, &owner);
    std::optional<AuthFailure> failure;
    auto handle = transport.start(
        AuthOperation::Send, QStringLiteral("42"), QStringLiteral("hello"),
        QByteArray("SYNTHETIC_SESSION_CANARY"), QByteArray(43, 'r'),
        {.complete =
             [&](AuthResponse) {
                 FAIL();
             },
         .failed =
             [&](AuthFailure value) {
                 failure = value;
             }});
    ASSERT_TRUE(handle);
    drainEventsUntil([&] {
        return failure.has_value();
    });
    EXPECT_EQ(failure, AuthFailure::RedirectRejected);
}

TEST(RumbleAuthTransport, OversizedResponseFailsClosedWithoutExposingBody)
{
    QObject owner;
    CaptureAuthManager manager;
    manager.response = QByteArray(64 * 1024 + 1, 'x');
    RumbleQtAuthTransport transport(manager, &owner);
    std::optional<AuthFailure> failure;
    auto handle = transport.start(
        AuthOperation::Send, QStringLiteral("42"), QStringLiteral("hello"),
        QByteArray("SYNTHETIC_SESSION_CANARY"), QByteArray(43, 'r'),
        {.complete =
             [&](AuthResponse) {
                 FAIL();
             },
         .failed =
             [&](AuthFailure value) {
                 failure = value;
             }});
    ASSERT_TRUE(handle);
    drainEventsUntil([&] {
        return failure.has_value();
    });
    EXPECT_EQ(failure, AuthFailure::ResponseLimit);
}

TEST(RumbleEmoteEligibility,
     CachesOnlyAuthoritativeSelectedUserAndInvalidatesWithSession)
{
    FakeTransport transport;
    SessionController session(transport);
    ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
    session.validate({});
    transport.complete(200, R"({"user":{"id":"viewer","username":"Viewer"}})");
    ASSERT_EQ(session.state(), SessionState::Valid);
    EXPECT_FALSE(session.emoteEligibility(QStringLiteral("42")));

    session.ensureEmoteEligibility(QStringLiteral("42"));
    EXPECT_EQ(transport.lastOperation, AuthOperation::Eligibility);
    EXPECT_EQ(transport.lastStream, QStringLiteral("42"));
    EXPECT_EQ(transport.lastBearer,
              QByteArrayLiteral("SYNTHETIC_SESSION_CANARY"));
    transport.complete(
        200,
        "data: "
        "{\"type\":\"init\",\"data\":{\"users\":[{\"id\":\"other\",\"is_"
        "follower\":false,\"badges\":[]},{\"id\":\"viewer\",\"is_follower\":"
        "true,\"badges\":[\"recurring_subscription\"]}]}}\n\n",
        "text/event-stream; charset=utf-8");
    const auto eligibility = session.emoteEligibility(QStringLiteral("42"));
    ASSERT_TRUE(eligibility);
    EXPECT_TRUE(eligibility->following);
    EXPECT_TRUE(eligibility->subscriberOrAdmin);

    const auto starts = transport.starts;
    session.ensureEmoteEligibility(QStringLiteral("42"));
    EXPECT_EQ(transport.starts, starts);

    session.clear();
    EXPECT_FALSE(session.emoteEligibility(QStringLiteral("42")));
}

TEST(RumbleEmoteEligibility, MalformedUnknownAndRejectedResponsesFailClosed)
{
    FakeTransport transport;
    SessionController session(transport);
    ASSERT_TRUE(session.importSession("SYNTHETIC_SESSION_CANARY"));
    session.validate({});
    transport.complete(200, R"({"user":{"id":"viewer"}})");
    ASSERT_EQ(session.state(), SessionState::Valid);

    session.ensureEmoteEligibility(QStringLiteral("7"));
    transport.complete(200,
                       "data: "
                       "{\"type\":\"init\",\"data\":{\"users\":[{\"id\":"
                       "\"viewer\",\"badges\":[]}]}}\n\n",
                       "text/event-stream");
    EXPECT_FALSE(session.emoteEligibility(QStringLiteral("7")));

    session.ensureEmoteEligibility(QStringLiteral("8"));
    transport.complete(401, QByteArray{}, "text/event-stream");
    EXPECT_EQ(session.state(), SessionState::Empty);
    EXPECT_TRUE(session.accountId().isEmpty());
}

TEST(RumbleEmoteEligibility, OutstandingAndCachedStreamStateIsBounded)
{
    FakeTransport transport;
    SessionController session(transport);
    validate(transport, session);

    for (int stream = 1; stream <= 300; ++stream)
    {
        session.ensureEmoteEligibility(QString::number(stream));
    }
    // One identity probe plus the bounded number of per-stream reads.
    EXPECT_EQ(transport.starts, 257);
    EXPECT_EQ(transport.lastOperation, AuthOperation::Eligibility);
    EXPECT_EQ(transport.lastStream, QStringLiteral("256"));
}

TEST(RumbleAuthTransport,
     EligibilityUsesBoundedAuthenticatedEventStreamGetAndNoBody)
{
    QObject owner;
    CaptureAuthManager manager;
    manager.contentType = "text/event-stream";
    manager.response = "data: {\"type\":\"init\",\"data\":{\"users\":[]}}\n\n";
    RumbleQtAuthTransport transport(manager, &owner);
    std::optional<AuthResponse> response;
    auto handle =
        transport.start(AuthOperation::Eligibility, QStringLiteral("42"), {},
                        QByteArray("SYNTHETIC_SESSION_CANARY"), {},
                        {.complete =
                             [&](AuthResponse value) {
                                 response = std::move(value);
                             },
                         .failed =
                             [&](AuthFailure) {
                                 FAIL();
                             }});
    ASSERT_TRUE(handle);
    drainEventsUntil([&] {
        return response.has_value();
    });
    ASSERT_TRUE(response);
    EXPECT_EQ(manager.operation, QNetworkAccessManager::GetOperation);
    EXPECT_EQ(manager.request.url(),
              QUrl(QStringLiteral(
                  "https://web7.rumble.com/chat/api/chat/42/stream")));
    EXPECT_EQ(manager.request.rawHeader("Accept"), "text/event-stream");
    EXPECT_EQ(manager.request.rawHeader("Cookie"),
              "u_s=SYNTHETIC_SESSION_CANARY");
    EXPECT_EQ(manager.request.rawHeader("Origin"), "https://rumble.com");
    EXPECT_TRUE(manager.request.rawHeader("Content-Type").isEmpty());
    EXPECT_TRUE(manager.body.isEmpty());
    EXPECT_LE(response->body.size(), 1024 * 1024);
}
