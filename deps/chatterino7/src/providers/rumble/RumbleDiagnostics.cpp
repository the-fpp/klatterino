// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/rumble/RumbleDiagnostics.hpp"

#include "common/QLogging.hpp"

#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <utility>

namespace chatterino::rumble {
namespace {

QString stateToken(StatusState state)
{
    switch (state)
    {
        case StatusState::Offline:
            return QStringLiteral("offline");
        case StatusState::Connecting:
            return QStringLiteral("connecting");
        case StatusState::Connected:
            return QStringLiteral("connected");
        case StatusState::Backoff:
            return QStringLiteral("backoff");
        case StatusState::RateLimited:
            return QStringLiteral("rate-limited");
        case StatusState::Error:
            return QStringLiteral("error");
        case StatusState::Stopped:
            return QStringLiteral("stopped");
    }
    return QStringLiteral("unknown");
}

QString writeToken(StatusWrite write)
{
    switch (write)
    {
        case StatusWrite::Unavailable:
            return QStringLiteral("unavailable");
        case StatusWrite::Writable:
            return QStringLiteral("writable");
        case StatusWrite::Busy:
            return QStringLiteral("busy");
        case StatusWrite::RateLimited:
            return QStringLiteral("rate-limited");
        case StatusWrite::DestinationDenied:
            return QStringLiteral("destination-denied");
    }
    return QStringLiteral("unknown");
}

QString actionToken(const StatusSnapshot &snapshot)
{
    switch (snapshot.write)
    {
        case StatusWrite::DestinationDenied:
            return QStringLiteral(
                "use Rumble's interface for this destination");
        case StatusWrite::RateLimited:
            return QStringLiteral("wait for the provider retry delay");
        case StatusWrite::Busy:
            return QStringLiteral("wait for the current send to finish");
        case StatusWrite::Unavailable:
        case StatusWrite::Writable:
            break;
    }
    switch (snapshot.state)
    {
        case StatusState::Offline:
            return QStringLiteral(
                "wait for the bounded recheck or retry manually");
        case StatusState::Connecting:
            return QStringLiteral("wait for resolution or connection");
        case StatusState::Connected:
            if (snapshot.account == StatusAccount::NeedsValidation)
                return QStringLiteral(
                    "validate or re-import the session before sending");
            if (snapshot.account == StatusAccount::Validating)
                return QStringLiteral("wait for session validation");
            return QStringLiteral("none");
        case StatusState::Backoff:
            return QStringLiteral("wait for the bounded automatic retry");
        case StatusState::RateLimited:
            return QStringLiteral("wait for the provider retry delay");
        case StatusState::Error:
            return QStringLiteral("retry manually");
        case StatusState::Stopped:
            return QStringLiteral("reopen the channel");
    }
    return QStringLiteral("retry manually");
}

QString locatorToken(StatusLocator locator)
{
    switch (locator)
    {
        case StatusLocator::Channel:
            return QStringLiteral("public-channel");
        case StatusLocator::Embed:
            return QStringLiteral("public-embed");
        case StatusLocator::Stream:
            return QStringLiteral("public-stream");
    }
    return QStringLiteral("unknown");
}

QString accountToken(StatusAccount account)
{
    switch (account)
    {
        case StatusAccount::LoggedOut:
            return QStringLiteral("logged-out");
        case StatusAccount::NeedsValidation:
            return QStringLiteral("needs-validation");
        case StatusAccount::Validating:
            return QStringLiteral("validating");
        case StatusAccount::Authenticated:
            return QStringLiteral("authenticated");
    }
    return QStringLiteral("unknown");
}

QString failureToken(RumbleFailureCategory category)
{
    switch (category)
    {
        case RumbleFailureCategory::Resolution:
            return QStringLiteral("resolution");
        case RumbleFailureCategory::Transport:
            return QStringLiteral("transport");
        case RumbleFailureCategory::Protocol:
            return QStringLiteral("protocol");
        case RumbleFailureCategory::Authentication:
            return QStringLiteral("authentication");
        case RumbleFailureCategory::Internal:
            return QStringLiteral("internal");
    }
    return QStringLiteral("unknown");
}

QString retryToken(RumbleRetryCause cause)
{
    switch (cause)
    {
        case RumbleRetryCause::ResolutionFailure:
            return QStringLiteral("resolution");
        case RumbleRetryCause::TransportFailure:
            return QStringLiteral("transport");
        case RumbleRetryCause::StreamEnded:
            return QStringLiteral("stream-ended");
        case RumbleRetryCause::Timeout:
            return QStringLiteral("timeout");
        case RumbleRetryCause::RateLimited:
            return QStringLiteral("rate-limit");
        case RumbleRetryCause::HttpFailure:
            return QStringLiteral("http");
        case RumbleRetryCause::ProtocolFailure:
            return QStringLiteral("protocol");
        case RumbleRetryCause::MissingInit:
            return QStringLiteral("missing-init");
        case RumbleRetryCause::HandoffLimit:
            return QStringLiteral("handoff-limit");
        case RumbleRetryCause::SchedulerUnavailable:
            return QStringLiteral("scheduler");
        case RumbleRetryCause::DeadlineOverflow:
            return QStringLiteral("deadline");
        case RumbleRetryCause::InvalidMetadata:
            return QStringLiteral("metadata");
        case RumbleRetryCause::Cancelled:
            return QStringLiteral("cancelled");
    }
    return QStringLiteral("unknown");
}

StatusState statusState(const RumbleLifecycleSnapshot &lifecycle)
{
    switch (lifecycle.state)
    {
        case RumbleChannelState::Unresolved:
        case RumbleChannelState::Connecting:
            return StatusState::Connecting;
        case RumbleChannelState::Offline:
            return StatusState::Offline;
        case RumbleChannelState::Connected:
            return StatusState::Connected;
        case RumbleChannelState::Backoff:
            return lifecycle.metadata.rateLimited ? StatusState::RateLimited
                                                  : StatusState::Backoff;
        case RumbleChannelState::Failed:
            return StatusState::Error;
        case RumbleChannelState::Closed:
            return StatusState::Stopped;
    }
    return StatusState::Error;
}

StatusLocator statusLocator(RumbleChannelKeyKind kind)
{
    switch (kind)
    {
        case RumbleChannelKeyKind::ChannelSlug:
            return StatusLocator::Channel;
        case RumbleChannelKeyKind::EmbedId:
            return StatusLocator::Embed;
        case RumbleChannelKeyKind::StreamId:
            return StatusLocator::Stream;
    }
    return StatusLocator::Channel;
}

StatusAccount statusAccount(std::optional<SessionState> state)
{
    if (!state)
        return StatusAccount::LoggedOut;
    switch (*state)
    {
        case SessionState::Empty:
            return StatusAccount::LoggedOut;
        case SessionState::Unvalidated:
            return StatusAccount::NeedsValidation;
        case SessionState::Validating:
            return StatusAccount::Validating;
        case SessionState::Valid:
            return StatusAccount::Authenticated;
    }
    return StatusAccount::LoggedOut;
}

}  // namespace

std::optional<StatusSnapshot> captureStatus(const RumbleChannel &channel,
                                            QDateTime nowUtc)
{
    if (!nowUtc.isValid())
        nowUtc = QDateTime::currentDateTimeUtc();
    nowUtc = nowUtc.toUTC();
    const auto data = channel.diagnosticData(nowUtc);
    if (!data)
        return std::nullopt;
    auto state = statusState(data->lifecycle);
    auto retryWaitMs = data->lifecycle.state == RumbleChannelState::Backoff
                           ? data->retryWaitMs
                           : std::nullopt;
    auto retryCause = data->lifecycle.state == RumbleChannelState::Backoff
                          ? data->lifecycle.metadata.retryCause
                          : std::nullopt;
    auto write = StatusWrite::Unavailable;
    const auto account = statusAccount(data->sessionState);
    const bool authenticated = account == StatusAccount::Authenticated;
    const bool connected =
        data->lifecycle.state == RumbleChannelState::Connected;
    if (connected && data->sessionBlockedUntilUtc &&
        data->sessionBlockedUntilUtc->isValid() &&
        data->sessionBlockedUntilUtc->toUTC() > nowUtc)
    {
        constexpr std::int64_t MAX_PUBLIC_WAIT_MS = 24LL * 60 * 60 * 1000;
        state = StatusState::RateLimited;
        write = StatusWrite::RateLimited;
        retryWaitMs = std::clamp<std::int64_t>(
            nowUtc.msecsTo(data->sessionBlockedUntilUtc->toUTC()), 0,
            MAX_PUBLIC_WAIT_MS);
        retryCause = RumbleRetryCause::RateLimited;
    }
    else if (connected && authenticated && data->destinationDenied)
    {
        write = StatusWrite::DestinationDenied;
    }
    else if (connected && authenticated && data->sendInProgress)
    {
        write = StatusWrite::Busy;
    }
    else if (connected && authenticated && data->hasDestination)
    {
        write = StatusWrite::Writable;
    }
    StatusSnapshot result{
        .state = state,
        .locator = statusLocator(data->locatorKind),
        .account = account,
        .write = write,
        .consecutiveFailures = data->lifecycle.metadata.consecutiveFailures,
        .retryWaitMs = retryWaitMs,
        .retryCause = retryCause,
        .lastError = data->lastFailure,
        .lastErrorAtUtc = data->lastFailureAtUtc,
    };
    return result;
}

QString formatStatus(const StatusSnapshot &snapshot)
{
    QStringList lines{
        QStringLiteral("Rumble status (safe to share)"),
        QStringLiteral("provider: rumble"),
        QStringLiteral("locator: %1").arg(locatorToken(snapshot.locator)),
        QStringLiteral("state: %1").arg(stateToken(snapshot.state)),
        QStringLiteral("next-action: %1").arg(actionToken(snapshot)),
        QStringLiteral("account: %1").arg(accountToken(snapshot.account)),
        QStringLiteral("write: %1").arg(writeToken(snapshot.write)),
        QStringLiteral("consecutive-failures: %1")
            .arg(snapshot.consecutiveFailures),
        QStringLiteral("retry-wait-ms: %1")
            .arg(snapshot.retryWaitMs
                     ? QString::number(std::clamp<std::int64_t>(
                           *snapshot.retryWaitMs, 0, 24LL * 60 * 60 * 1000))
                     : QStringLiteral("none")),
        QStringLiteral("retry-cause: %1")
            .arg(snapshot.retryCause ? retryToken(*snapshot.retryCause)
                                     : QStringLiteral("none")),
        QStringLiteral("last-error: %1")
            .arg(snapshot.lastError ? failureToken(*snapshot.lastError)
                                    : QStringLiteral("none")),
        QStringLiteral("last-error-at: %1")
            .arg(snapshot.lastErrorAtUtc && snapshot.lastErrorAtUtc->isValid()
                     ? snapshot.lastErrorAtUtc->toUTC().toString(Qt::ISODate)
                     : QStringLiteral("none")),
    };
    return lines.join(QLatin1Char('\n'));
}

QString sanitizeStatusToken(QString value)
{
    if (value.size() > 64)
        return QStringLiteral("unknown");
    static constexpr std::array<const char *, 34> ALLOWED{
        "rumble",
        "public-channel",
        "public-embed",
        "public-stream",
        "offline",
        "connecting",
        "connected",
        "backoff",
        "rate-limited",
        "error",
        "stopped",
        "logged-out",
        "needs-validation",
        "validating",
        "authenticated",
        "resolution",
        "transport",
        "protocol",
        "authentication",
        "internal",
        "stream-ended",
        "timeout",
        "rate-limit",
        "http",
        "missing-init",
        "handoff-limit",
        "scheduler",
        "deadline",
        "metadata",
        "cancelled",
        "unavailable",
        "writable",
        "busy",
        "destination-denied",
    };
    value = value.trimmed().toLower();
    const auto found =
        std::ranges::find_if(ALLOWED, [&value](const char *item) {
            return value == QString::fromLatin1(item);
        });
    return found == ALLOWED.end() ? QStringLiteral("unknown") : value;
}

StatusLogCoalescer::StatusLogCoalescer(std::int64_t intervalMs,
                                       std::size_t maximumKeys)
    : intervalMs_(
          std::clamp<std::int64_t>(intervalMs, 1, 24LL * 60 * 60 * 1000))
    , maximumKeys_(std::clamp<std::size_t>(maximumKeys, 1, 1024))
{
    entries_.reserve(maximumKeys_);
}

StatusLogDecision StatusLogCoalescer::observe(StatusLogKey key,
                                              std::int64_t nowMs)
{
    const auto saturatingIncrement = [](std::uint32_t &value) {
        if (value != std::numeric_limits<std::uint32_t>::max())
            ++value;
    };
    const auto saturatingAdd = [](std::uint32_t left, std::uint32_t right) {
        const auto maximum = std::numeric_limits<std::uint32_t>::max();
        return maximum - left < right ? maximum : left + right;
    };
    if (lastNowMs_)
        nowMs = std::max(nowMs, *lastNowMs_);
    lastNowMs_ = nowMs;
    const auto intervalElapsed = [this](std::int64_t since, std::int64_t now) {
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        return since <= maximum - intervalMs_ && now >= since + intervalMs_;
    };
    if (emissionsInWindow_ == 0 || intervalElapsed(windowStartMs_, nowMs))
    {
        windowStartMs_ = nowMs;
        emissionsInWindow_ = 0;
    }
    auto found = std::ranges::find_if(entries_, [&key](const Entry &entry) {
        return entry.key == key;
    });
    if (found != entries_.end())
    {
        found->lastObservationMs = nowMs;
        if (!intervalElapsed(found->lastEmissionMs, nowMs))
        {
            saturatingIncrement(found->suppressed);
            return {};
        }
        if (emissionsInWindow_ >= maximumKeys_)
        {
            saturatingIncrement(found->suppressed);
            return {};
        }
        ++emissionsInWindow_;
        const auto suppressed =
            saturatingAdd(std::exchange(found->suppressed, 0),
                          std::exchange(carriedSuppressed_, 0));
        found->lastEmissionMs = nowMs;
        return {.emit = true, .suppressed = suppressed};
    }

    if (entries_.size() == maximumKeys_)
    {
        found =
            std::ranges::min_element(entries_, {}, &Entry::lastObservationMs);
        carriedSuppressed_ =
            saturatingAdd(carriedSuppressed_, found->suppressed);
        *found = Entry{std::move(key), nowMs, nowMs, 0};
    }
    else
    {
        entries_.push_back(Entry{std::move(key), nowMs, nowMs, 0});
        found = std::prev(entries_.end());
    }
    if (emissionsInWindow_ >= maximumKeys_)
    {
        saturatingIncrement(found->suppressed);
        return {};
    }
    ++emissionsInWindow_;
    return {.emit = true, .suppressed = std::exchange(carriedSuppressed_, 0)};
}

std::size_t StatusLogCoalescer::size() const noexcept
{
    return entries_.size();
}

void logStatus(const StatusSnapshot &snapshot, std::int64_t nowMs)
{
    static QMutex mutex;
    static StatusLogCoalescer coalescer;
    StatusLogDecision decision;
    {
        QMutexLocker guard(&mutex);
        decision = coalescer.observe(
            {snapshot.state, snapshot.account, snapshot.write, snapshot.locator,
             snapshot.lastError, snapshot.retryCause},
            nowMs);
    }
    if (!decision.emit)
        return;
    qCInfo(chatterinoRumble).noquote()
        << QStringLiteral("state=%1 account=%2 write=%3 locator=%4 failures=%5 "
                          "retry=%6 error=%7 suppressed=%8")
               .arg(stateToken(snapshot.state), accountToken(snapshot.account),
                    writeToken(snapshot.write), locatorToken(snapshot.locator),
                    QString::number(snapshot.consecutiveFailures),
                    snapshot.retryCause ? retryToken(*snapshot.retryCause)
                                        : QStringLiteral("none"),
                    snapshot.lastError ? failureToken(*snapshot.lastError)
                                       : QStringLiteral("none"),
                    QString::number(decision.suppressed));
}

}  // namespace chatterino::rumble
