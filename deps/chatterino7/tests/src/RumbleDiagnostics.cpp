// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleDiagnostics.hpp"

#include "controllers/commands/builtin/rumble/Status.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/CommandController.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Logging.hpp"
#include "providers/rumble/RumbleChannelProvider.hpp"
#include "providers/rumble/RumbleDispatcher.hpp"
#include "Test.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QtLogging>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace chatterino {
namespace {

class NoopHandle final : public rumble::AuthHandle
{
public:
    void cancel() noexcept override
    {
    }
};

class DiagnosticApplication final : public mock::BaseApplication
{
public:
    DiagnosticApplication()
        : commands(paths_)
    {
    }

    CommandController *getCommands() override
    {
        return &commands;
    }

    EmoteController *getEmotes() override
    {
        return &emotes;
    }

    ILogging *getChatLogger() override
    {
        return &logging;
    }

    mock::EmptyLogging logging;
    CommandController commands;
    mock::EmoteController emotes;
};

class AcceptingAuthTransport final : public rumble::AuthTransport
{
public:
    std::unique_ptr<rumble::AuthHandle> start(
        rumble::AuthOperation operation, QString, QString, QByteArray,
        QByteArray, rumble::AuthCallbacks callbacks) override
    {
        if (operation == rumble::AuthOperation::Probe)
        {
            callbacks.complete({200,
                                "application/json",
                                R"({"user":{"id":"hostile-private-id"}})",
                                {}});
        }
        else
        {
            pendingSend = std::move(callbacks);
        }
        return std::make_unique<NoopHandle>();
    }

    void completeSend(int status,
                      std::optional<QByteArray> retryAfter = std::nullopt)
    {
        auto complete = std::move(pendingSend.complete);
        pendingSend = {};
        ASSERT_TRUE(complete);
        complete({status, "application/json", "{}", std::move(retryAfter)});
    }

    rumble::AuthCallbacks pendingSend;
};

struct CapturedLog {
    QtMsgType type = QtDebugMsg;
    QString category;
    QString message;
};

std::mutex messageHandlerMutex;
std::vector<CapturedLog> *messageHandlerTarget = nullptr;

void captureMessageHandler(QtMsgType type, const QMessageLogContext &context,
                           const QString &message)
{
    std::lock_guard guard(messageHandlerMutex);
    if (messageHandlerTarget)
    {
        messageHandlerTarget->push_back(
            {type,
             context.category ? QString::fromUtf8(context.category) : QString{},
             message});
    }
}

class ScopedMessageCapture
{
public:
    ScopedMessageCapture()
    {
        std::lock_guard guard(messageHandlerMutex);
        messageHandlerTarget = &records_;
        previous_ = qInstallMessageHandler(captureMessageHandler);
    }

    ~ScopedMessageCapture()
    {
        qInstallMessageHandler(previous_);
        std::lock_guard guard(messageHandlerMutex);
        messageHandlerTarget = nullptr;
    }

    std::vector<CapturedLog> records() const
    {
        std::lock_guard guard(messageHandlerMutex);
        return records_;
    }

private:
    QtMessageHandler previous_ = nullptr;
    std::vector<CapturedLog> records_;
};

RumbleChannelKey key(RumbleChannelKeyKind kind, QString value)
{
    auto result = RumbleChannelKey::normalize(kind, std::move(value));
    EXPECT_TRUE(result);
    return std::move(*result);
}

void connectStream(const std::shared_ptr<RumbleChannel> &channel,
                   QString streamId = QStringLiteral("42"))
{
    auto operation = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(operation);
    auto metadata = RumbleResolvedMetadata::create(
        QStringLiteral("Private display"), std::nullopt, std::nullopt,
        key(RumbleChannelKeyKind::StreamId, std::move(streamId)));
    ASSERT_TRUE(metadata);
    channel->publishMetadata(*operation, std::move(*metadata));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connecting));
    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Connected));
}

QByteArray readRepositoryFile(const QString &relativePath)
{
    const QDir repository(QStringLiteral(CHATTERINO_PLUGIN_REPOSITORY_ROOT));
    QFile file(repository.filePath(relativePath));
    EXPECT_TRUE(file.open(QFile::ReadOnly)) << file.fileName().toStdString();
    return file.readAll();
}

rumble::StatusSnapshot snapshot(rumble::StatusState state)
{
    return {
        .state = state,
        .locator = rumble::StatusLocator::Channel,
        .account = rumble::StatusAccount::LoggedOut,
        .write = rumble::StatusWrite::Unavailable,
    };
}

}  // namespace

TEST(RumbleDiagnostic, FormatsEveryFinalStateWithAction)
{
    using S = rumble::StatusState;
    constexpr std::array states{S::Offline, S::Connecting,  S::Connected,
                                S::Backoff, S::RateLimited, S::Error,
                                S::Stopped};
    constexpr std::array expected{"state: offline",      "state: connecting",
                                  "state: connected",    "state: backoff",
                                  "state: rate-limited", "state: error",
                                  "state: stopped"};
    for (std::size_t index = 0; index < states.size(); ++index)
    {
        const auto formatted = rumble::formatStatus(snapshot(states[index]));
        EXPECT_TRUE(formatted.contains(QString::fromLatin1(expected[index])));
        EXPECT_TRUE(formatted.contains(QStringLiteral("next-action: ")));
        EXPECT_TRUE(formatted.startsWith(
            QStringLiteral("Rumble status (safe to share)")));
    }
}

TEST(RumbleDiagnostic, LifecycleActionTakesPriorityOverOptionalAccountState)
{
    auto error = snapshot(rumble::StatusState::Error);
    error.account = rumble::StatusAccount::NeedsValidation;
    EXPECT_TRUE(rumble::formatStatus(error).contains(
        QStringLiteral("next-action: retry manually")));

    auto stopped = snapshot(rumble::StatusState::Stopped);
    stopped.account = rumble::StatusAccount::Validating;
    EXPECT_TRUE(rumble::formatStatus(stopped).contains(
        QStringLiteral("next-action: reopen the channel")));
}

TEST(RumbleDiagnostic, BoundsCountdownAndFormatsOnlySanitizedScalars)
{
    auto value = snapshot(rumble::StatusState::RateLimited);
    value.locator = rumble::StatusLocator::Embed;
    value.account = rumble::StatusAccount::Authenticated;
    value.consecutiveFailures = 12;
    value.retryWaitMs = std::numeric_limits<std::int64_t>::max();
    value.retryCause = RumbleRetryCause::RateLimited;
    value.lastError = RumbleFailureCategory::Protocol;
    value.lastErrorAtUtc = QDateTime::fromString(
        QStringLiteral("2026-07-17T12:00:00Z"), Qt::ISODate);
    const auto formatted = rumble::formatStatus(value);
    EXPECT_TRUE(formatted.contains(QStringLiteral("locator: public-embed")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("account: authenticated")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("retry-wait-ms: 86400000")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("retry-cause: rate-limit")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("last-error: protocol")));
    EXPECT_TRUE(formatted.contains(
        QStringLiteral("last-error-at: 2026-07-17T12:00:00Z")));
}

TEST(RumbleRedaction, RejectsHostileValuesInsteadOfReflectingThem)
{
    const std::array hostile{
        QStringLiteral("https://user:pass@rumble.com/c/private?u_s=secret#x"),
        QStringLiteral("Authorization: Bearer secret"),
        QStringLiteral("Cookie: u_s=secret"),
        QStringLiteral(R"({"user":{"id":"private"},"message":"secret"})"),
        QStringLiteral("request-id\nusername\tsecret") + QChar(u'\0'),
        QString::fromUtf8("\xF0\x9F\x94\x90 private-user"),
    };
    for (const auto &input : hostile)
    {
        EXPECT_EQ(rumble::sanitizeStatusToken(input),
                  QStringLiteral("unknown"));
    }
    EXPECT_EQ(rumble::sanitizeStatusToken(QStringLiteral(" CONNECTED ")),
              QStringLiteral("connected"));
    EXPECT_EQ(rumble::sanitizeStatusToken(QString(10000, QChar(u'A'))),
              QStringLiteral("unknown"));
}

TEST(RumbleStatusCommand, CaptureAndFormattingAreIdempotentAndLocalOnly)
{
    DiagnosticApplication app;
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("private-channel"));
    ASSERT_TRUE(created);
    auto channel = *created;
    AcceptingAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession(QByteArrayLiteral("private-session")));
    channel->setSessionController(session);

    const auto beforeLifecycle = channel->lifecycleSnapshot();
    const auto beforeSessionGeneration = session->generation();
    const auto now = QDateTime::fromMSecsSinceEpoch(100000, Qt::UTC);
    const auto first = rumble::captureStatus(*channel, now);
    const auto second = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->state, rumble::StatusState::Connecting);
    EXPECT_EQ(first->account, rumble::StatusAccount::NeedsValidation);
    EXPECT_EQ(channel->lifecycleSnapshot(), beforeLifecycle);
    EXPECT_EQ(session->generation(), beforeSessionGeneration);
    const auto output = rumble::formatStatus(*first);
    EXPECT_FALSE(output.contains(QStringLiteral("private-channel")));
    EXPECT_FALSE(output.contains(QStringLiteral("private-session")));

    const auto messageCount = channel->getMessageSnapshot().size();
    EXPECT_TRUE(
        app.getCommands()
            ->execCommand(QStringLiteral("/rumble-status"), channel, false)
            .isEmpty());
    const auto messages = channel->getMessageSnapshot();
    ASSERT_EQ(messages.size(), messageCount + 1);
    EXPECT_TRUE(messages.back()->messageText.startsWith(
        QStringLiteral("Rumble status (safe to share)")));
    EXPECT_EQ(channel->lifecycleSnapshot(), beforeLifecycle);
    EXPECT_EQ(session->generation(), beforeSessionGeneration);

    bool accepted = false;
    session->validate([&](bool valid, QString) {
        accepted = valid;
    });
    ASSERT_TRUE(accepted);
    const auto authenticated = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(authenticated);
    EXPECT_EQ(authenticated->account, rumble::StatusAccount::Authenticated);
    EXPECT_FALSE(rumble::formatStatus(*authenticated)
                     .contains(QStringLiteral("hostile-private-id")));
}

TEST(RumbleStatusCommand, RejectsMissingAndNonRumbleChannelsLocally)
{
    DiagnosticApplication app;
    CommandContext missing{
        .words = {QStringLiteral("/rumble-status")},
    };
    EXPECT_TRUE(commands::rumbleStatus(missing).isEmpty());

    auto nonRumble = std::make_shared<Channel>(QStringLiteral("private"),
                                               Channel::Type::None);
    CommandContext wrong{
        .words = {QStringLiteral("/rumble-status")},
        .channel = nonRumble,
    };
    EXPECT_TRUE(commands::rumbleStatus(wrong).isEmpty());
    const auto messages = nonRumble->getMessageSnapshot();
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(
        messages.front()->messageText,
        QStringLiteral(
            "The /rumble-status command only works in a Rumble channel."));
}

TEST(RumbleDiagnostic, RetainsOnlyLastSanitizedFailureCategoryAndTime)
{
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("secret-slug"));
    ASSERT_TRUE(created);
    auto channel = *created;
    ASSERT_TRUE(channel->transitionTo(
        RumbleChannelState::Failed,
        RumbleFailure{RumbleFailureCategory::Protocol,
                      RumbleFailureCode::MalformedResponse,
                      RumbleOperatorText::ResponseContractChanged}));
    const auto status =
        rumble::captureStatus(*channel, QDateTime::currentDateTimeUtc());
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, rumble::StatusState::Error);
    EXPECT_EQ(status->lastError, RumbleFailureCategory::Protocol);
    ASSERT_TRUE(status->lastErrorAtUtc);
    EXPECT_TRUE(status->lastErrorAtUtc->isValid());
    const auto output = rumble::formatStatus(*status);
    EXPECT_FALSE(output.contains(QStringLiteral("secret-slug")));
    EXPECT_FALSE(output.contains(
        QStringLiteral("The response contract was not recognized")));
}

TEST(RumbleDiagnostic, ReportsBoundedRateLimitCountdownFromCopiedMetadata)
{
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("private-rate-limit"));
    ASSERT_TRUE(created);
    auto channel = *created;
    auto operation = channel->beginOperation(RumbleOperationKind::Connection);
    ASSERT_TRUE(operation);
    const auto before = QDateTime::currentDateTimeUtc();
    channel->publishLifecycle(*operation, RumbleChannelState::Backoff,
                              {.consecutiveFailures = 1,
                               .scheduledAtMs = 100,
                               .deadlineAtMs = 5100,
                               .rateLimited = true,
                               .retryCause = RumbleRetryCause::RateLimited});

    const auto initial = rumble::captureStatus(*channel, before);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(initial->retryWaitMs);
    EXPECT_EQ(*initial->retryWaitMs, 5000);
    EXPECT_EQ(initial->state, rumble::StatusState::RateLimited);
    EXPECT_EQ(initial->lastError, RumbleFailureCategory::Transport);
    const auto later = rumble::captureStatus(*channel, before.addMSecs(2000));
    ASSERT_TRUE(later);
    ASSERT_TRUE(later->retryWaitMs);
    EXPECT_GE(*later->retryWaitMs, 2900);
    EXPECT_LE(*later->retryWaitMs, 3100);
    channel->close();
    const auto stopped = rumble::captureStatus(*channel, before.addMSecs(3000));
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->state, rumble::StatusState::Stopped);
    EXPECT_FALSE(stopped->retryWaitMs);
    EXPECT_FALSE(stopped->retryCause);
}

TEST(RumbleDiagnostic, ReportsSessionSendRateLimitWithBoundedCountdown)
{
    const auto now = QDateTime::fromMSecsSinceEpoch(100000, Qt::UTC);
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("private-429"));
    ASSERT_TRUE(created);
    auto channel = *created;
    AcceptingAuthTransport transport;
    auto session =
        std::make_shared<rumble::SessionController>(transport, [now] {
            return now;
        });
    ASSERT_TRUE(session->importSession(QByteArrayLiteral("private-session")));
    session->validate({});
    ASSERT_EQ(session->state(), rumble::SessionState::Valid);
    channel->setSessionController(session);
    connectStream(channel);

    session->send(QStringLiteral("42"), QStringLiteral("hello"), {});
    transport.completeSend(429, QByteArrayLiteral("120"));
    const auto status = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, rumble::StatusState::RateLimited);
    EXPECT_EQ(status->account, rumble::StatusAccount::Authenticated);
    EXPECT_EQ(status->write, rumble::StatusWrite::RateLimited);
    EXPECT_EQ(status->retryWaitMs, 120000);
    EXPECT_EQ(status->retryCause, RumbleRetryCause::RateLimited);
    channel->close();
    const auto stopped = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->state, rumble::StatusState::Stopped);
    EXPECT_EQ(stopped->write, rumble::StatusWrite::Unavailable);
    EXPECT_FALSE(stopped->retryWaitMs);
    EXPECT_FALSE(stopped->retryCause);
}

TEST(RumbleDiagnostic, ReportsStreamSpecificSendRejectionWithoutIdentity)
{
    const auto now = QDateTime::fromMSecsSinceEpoch(100000, Qt::UTC);
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("private-403"));
    ASSERT_TRUE(created);
    auto channel = *created;
    AcceptingAuthTransport transport;
    auto session =
        std::make_shared<rumble::SessionController>(transport, [now] {
            return now;
        });
    ASSERT_TRUE(session->importSession(QByteArrayLiteral("private-session")));
    session->validate({});
    channel->setSessionController(session);
    connectStream(channel, QStringLiteral("4042"));

    session->send(QStringLiteral("4042"), QStringLiteral("hello"), {});
    transport.completeSend(403);
    const auto status = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, rumble::StatusState::Connected);
    EXPECT_EQ(status->account, rumble::StatusAccount::Authenticated);
    EXPECT_EQ(status->write, rumble::StatusWrite::DestinationDenied);
    const auto output = rumble::formatStatus(*status);
    EXPECT_TRUE(output.contains(QStringLiteral("write: destination-denied")));
    EXPECT_FALSE(output.contains(QStringLiteral("4042")));
    EXPECT_FALSE(output.contains(QStringLiteral("private-403")));

    session->send(QStringLiteral("43"), QStringLiteral("hello"), {});
    transport.completeSend(429, QByteArrayLiteral("60"));
    const auto blocked = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked->state, rumble::StatusState::RateLimited);
    EXPECT_EQ(blocked->write, rumble::StatusWrite::RateLimited);
    EXPECT_EQ(blocked->retryWaitMs, 60000);

    ASSERT_TRUE(channel->transitionTo(RumbleChannelState::Offline));
    const auto offline = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(offline);
    EXPECT_EQ(offline->state, rumble::StatusState::Offline);
    EXPECT_EQ(offline->write, rumble::StatusWrite::Unavailable);
    EXPECT_FALSE(offline->retryWaitMs);
    EXPECT_FALSE(offline->retryCause);
}

TEST(RumbleDiagnostic, ReportsSend401AsNeedingValidation)
{
    const auto now = QDateTime::fromMSecsSinceEpoch(100000, Qt::UTC);
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                        QStringLiteral("private-401"));
    ASSERT_TRUE(created);
    auto channel = *created;
    AcceptingAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession(QByteArrayLiteral("private-session")));
    session->validate({});
    channel->setSessionController(session);
    connectStream(channel);

    session->send(QStringLiteral("42"), QStringLiteral("hello"), {});
    transport.completeSend(401);
    const auto status = rumble::captureStatus(*channel, now);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->account, rumble::StatusAccount::NeedsValidation);
    EXPECT_EQ(status->write, rumble::StatusWrite::Unavailable);
    EXPECT_TRUE(rumble::formatStatus(*status).contains(QStringLiteral(
        "next-action: validate or re-import the session before sending")));
}

TEST(RumbleDiagnostic, OwnerBoundaryAndCopiedSnapshotSurviveChannelDestruction)
{
    const auto now = QDateTime::fromMSecsSinceEpoch(100000, Qt::UTC);
    rumble::StatusSnapshot retained;
    std::weak_ptr<RumbleChannel> weak;
    {
        QObject owner;
        auto dispatcher = makeQtRumbleDispatcher(&owner);
        RumbleChannelProvider provider(dispatcher);
        auto created = provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                                            QStringLiteral("private-lifetime"));
        ASSERT_TRUE(created);
        auto channel = *created;
        const auto ownerStatus = rumble::captureStatus(*channel, now);
        ASSERT_TRUE(ownerStatus);

        std::optional<rumble::StatusSnapshot> offOwnerStatus;
        std::thread worker([channel, now, &offOwnerStatus] {
            offOwnerStatus = rumble::captureStatus(*channel, now);
        });
        worker.join();
        EXPECT_FALSE(offOwnerStatus);

        channel->close();
        const auto stopped = rumble::captureStatus(*channel, now);
        ASSERT_TRUE(stopped);
        EXPECT_EQ(stopped->state, rumble::StatusState::Stopped);
        retained = *stopped;
        weak = channel;
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(rumble::formatStatus(retained).contains(
        QStringLiteral("state: stopped")));
}

TEST(RumbleRedaction, ActualTransitionLogUsesOnlyClosedVocabulary)
{
    QObject owner;
    auto dispatcher = makeQtRumbleDispatcher(&owner);
    RumbleChannelProvider provider(dispatcher);
    auto created =
        provider.getOrCreate(RumbleChannelKeyKind::ChannelSlug,
                             QStringLiteral("private-log-cookie-u_s-secret"));
    ASSERT_TRUE(created);
    auto channel = *created;
    AcceptingAuthTransport transport;
    auto session = std::make_shared<rumble::SessionController>(transport);
    ASSERT_TRUE(session->importSession(
        QByteArrayLiteral("SYNTHETIC_PRIVATE_SESSION_CANARY")));
    session->validate({});
    channel->setSessionController(session);

    ScopedMessageCapture capture;
    ASSERT_TRUE(channel->transitionTo(
        RumbleChannelState::Failed,
        RumbleFailure{RumbleFailureCategory::Protocol,
                      RumbleFailureCode::MalformedResponse,
                      RumbleOperatorText::ResponseContractChanged}));
    const auto records = capture.records();
    const auto found =
        std::ranges::find_if(records, [](const CapturedLog &log) {
            return log.type == QtInfoMsg &&
                   log.category == QStringLiteral("chatterino.rumble") &&
                   log.message.contains(QStringLiteral("state=error"));
        });
    ASSERT_NE(found, records.end());
    EXPECT_TRUE(
        found->message.contains(QStringLiteral("account=authenticated")));
    EXPECT_TRUE(found->message.contains(QStringLiteral("write=unavailable")));
    EXPECT_TRUE(found->message.contains(QStringLiteral("error=protocol")));
    EXPECT_TRUE(found->message.contains(QStringLiteral("suppressed=")));
    for (const auto &canary :
         {QStringLiteral("private-log-cookie-u_s-secret"),
          QStringLiteral("SYNTHETIC_PRIVATE_SESSION_CANARY"),
          QStringLiteral("hostile-private-id"),
          QStringLiteral("response contract"), QStringLiteral("Cookie:"),
          QStringLiteral("Authorization:")})
    {
        EXPECT_FALSE(found->message.contains(canary, Qt::CaseInsensitive));
    }
}

TEST(RumbleDiagnostic, UserGuideCommandsAndLinksStayConsistent)
{
    const auto readme = readRepositoryFile(QStringLiteral("README.md"));
    const auto guidePath =
        QStringLiteral("deps/chatterino7/docs/rumble-user-guide.md");
    const auto guide = readRepositoryFile(guidePath);
    EXPECT_TRUE(readme.contains(
        "[Rumble user guide](deps/chatterino7/docs/rumble-user-guide.md)"));
    EXPECT_TRUE(guide.contains("/rumble-status"));
    EXPECT_TRUE(guide.contains(
        "nix run 'github:the-fpp/klatterino#rumble-diagnose-channel'"));
    EXPECT_TRUE(guide.contains("--channel CHANNEL_SLUG"));

    const QDir guideDirectory(
        QFileInfo(QDir(QStringLiteral(CHATTERINO_PLUGIN_REPOSITORY_ROOT))
                      .filePath(guidePath))
            .absolutePath());
    for (const auto &target :
         {QStringLiteral("rumble-moderation.md"),
          QStringLiteral("rumble-protocol.md"),
          QStringLiteral("rumble-lifecycle.md"),
          QStringLiteral("../tests/fixtures/rumble/README.md")})
    {
        EXPECT_TRUE(QFileInfo::exists(guideDirectory.filePath(target)))
            << target.toStdString();
    }
}

TEST(RumbleLogCoalescing, CoalescesExpiresAndReportsSuppression)
{
    rumble::StatusLogCoalescer coalescer(1000, 8);
    const rumble::StatusLogKey key{
        rumble::StatusState::Backoff,     rumble::StatusAccount::LoggedOut,
        rumble::StatusWrite::Unavailable, rumble::StatusLocator::Channel,
        RumbleFailureCategory::Transport, RumbleRetryCause::Timeout};
    EXPECT_EQ(coalescer.observe(key, 100),
              (rumble::StatusLogDecision{true, 0}));
    EXPECT_EQ(coalescer.observe(key, 200),
              (rumble::StatusLogDecision{false, 0}));
    EXPECT_EQ(coalescer.observe(key, 300),
              (rumble::StatusLogDecision{false, 0}));
    EXPECT_EQ(coalescer.observe(key, 1100),
              (rumble::StatusLogDecision{true, 2}));
}

TEST(RumbleLogCoalescing, BoundsKeyMemoryAndGlobalEmissionBudget)
{
    rumble::StatusLogCoalescer coalescer(1000, 3);
    std::size_t emitted = 0;
    for (const auto state :
         {rumble::StatusState::Offline, rumble::StatusState::Connecting,
          rumble::StatusState::Connected, rumble::StatusState::Backoff,
          rumble::StatusState::RateLimited, rumble::StatusState::Error})
    {
        emitted += coalescer
                       .observe({state,
                                 rumble::StatusAccount::LoggedOut,
                                 rumble::StatusWrite::Unavailable,
                                 rumble::StatusLocator::Channel,
                                 {},
                                 {}},
                                1)
                       .emit;
        EXPECT_LE(coalescer.size(), 3U);
    }
    EXPECT_EQ(emitted, 3U);
    EXPECT_EQ(coalescer.size(), 3U);
}

TEST(RumbleLogCoalescing,
     AdversarialChurnCannotReemitOrLoseSuppressionInsideWindow)
{
    rumble::StatusLogCoalescer coalescer(1000, 3);
    const rumble::StatusLogKey hot{
        rumble::StatusState::Backoff,     rumble::StatusAccount::Authenticated,
        rumble::StatusWrite::RateLimited, rumble::StatusLocator::Stream,
        RumbleFailureCategory::Transport, RumbleRetryCause::RateLimited};
    EXPECT_TRUE(coalescer.observe(hot, 100).emit);
    EXPECT_FALSE(coalescer.observe(hot, 110).emit);

    EXPECT_TRUE(coalescer
                    .observe({rumble::StatusState::Offline,
                              rumble::StatusAccount::LoggedOut,
                              rumble::StatusWrite::Unavailable,
                              rumble::StatusLocator::Channel,
                              {},
                              {}},
                             120)
                    .emit);
    EXPECT_TRUE(coalescer
                    .observe({rumble::StatusState::Connecting,
                              rumble::StatusAccount::NeedsValidation,
                              rumble::StatusWrite::Unavailable,
                              rumble::StatusLocator::Embed,
                              {},
                              {}},
                             130)
                    .emit);
    EXPECT_FALSE(coalescer
                     .observe({rumble::StatusState::Error,
                               rumble::StatusAccount::Validating,
                               rumble::StatusWrite::Busy,
                               rumble::StatusLocator::Channel,
                               RumbleFailureCategory::Protocol,
                               {}},
                              140)
                     .emit);
    EXPECT_FALSE(coalescer.observe(hot, 150).emit);
    EXPECT_LE(coalescer.size(), 3U);

    const auto afterWindow = coalescer.observe(hot, 1200);
    EXPECT_TRUE(afterWindow.emit);
    EXPECT_GE(afterWindow.suppressed, 2U);
}

TEST(RumbleLogCoalescing, DoesNotHideAccountStateChanges)
{
    rumble::StatusLogCoalescer coalescer(1000, 8);
    EXPECT_TRUE(coalescer
                    .observe({rumble::StatusState::Connected,
                              rumble::StatusAccount::LoggedOut,
                              rumble::StatusWrite::Unavailable,
                              rumble::StatusLocator::Channel,
                              {},
                              {}},
                             100)
                    .emit);
    EXPECT_TRUE(coalescer
                    .observe({rumble::StatusState::Connected,
                              rumble::StatusAccount::Authenticated,
                              rumble::StatusWrite::Writable,
                              rumble::StatusLocator::Channel,
                              {},
                              {}},
                             101)
                    .emit);
}

TEST(RumbleLogCoalescing, ClockRollbackAndExtremesCannotBypassInterval)
{
    const rumble::StatusLogKey key{
        rumble::StatusState::Error,       rumble::StatusAccount::LoggedOut,
        rumble::StatusWrite::Unavailable, rumble::StatusLocator::Channel,
        RumbleFailureCategory::Internal,  {}};
    rumble::StatusLogCoalescer coalescer(1000, 8);
    EXPECT_TRUE(coalescer.observe(key, 100).emit);
    EXPECT_FALSE(coalescer.observe(key, 50).emit);
    EXPECT_FALSE(
        coalescer.observe(key, std::numeric_limits<std::int64_t>::min()).emit);
    EXPECT_FALSE(coalescer.observe(key, 1099).emit);
    const auto expired = coalescer.observe(key, 1100);
    EXPECT_TRUE(expired.emit);
    EXPECT_EQ(expired.suppressed, 3U);

    rumble::StatusLogCoalescer extremes(1000, 8);
    EXPECT_TRUE(
        extremes.observe(key, std::numeric_limits<std::int64_t>::min()).emit);
    EXPECT_TRUE(
        extremes.observe(key, std::numeric_limits<std::int64_t>::max()).emit);
    EXPECT_FALSE(
        extremes.observe(key, std::numeric_limits<std::int64_t>::min()).emit);
}

}  // namespace chatterino
